/* -------------------------------------------------------------------------
 *
 * pg_keeper.c
 *
 * Simple clustering extension module for PostgreSQL.
 *
 * -------------------------------------------------------------------------
 */

#include "postgres.h"

#include "pg_keeper.h"
#include "util.h"
#include "syncrep.h"

/* These are always necessary for a bgworker */
#include "access/xlog.h"
#include "access/htup_details.h"
#include "access/reloptions.h"
#include "access/xact.h"
#include "executor/spi.h"
#include "miscadmin.h"
#include "postmaster/bgworker.h"
#include "replication/syncrep.h"
#include "storage/ipc.h"
#include "storage/latch.h"
#include "storage/lwlock.h"
#include "storage/proc.h"
#include "storage/shmem.h"
#include "utils/snapmgr.h"
#include "utils/builtins.h"
#include "utils/rel.h"

/* these headers are used by this particular worker's code */
#include "tcop/utility.h"
#include "libpq-int.h"

PG_MODULE_MAGIC;

PG_FUNCTION_INFO_V1(add_node);
PG_FUNCTION_INFO_V1(del_node);
PG_FUNCTION_INFO_V1(del_node_by_seqno);
PG_FUNCTION_INFO_V1(indirect_polling);
PG_FUNCTION_INFO_V1(indirect_kill);

void	_PG_init(void);
void	KeeperMain(Datum);

static void checkParameter(void);

static shmem_startup_hook_type prev_shmem_startup_hook = NULL;
static void pg_keeper_shmem_startup(void);

/* Function for signal handler */
static void pg_keeper_sigterm(SIGNAL_ARGS);
static void pg_keeper_sighup(SIGNAL_ARGS);
static void pg_keeper_sigusr1(SIGNAL_ARGS);

/* flags set by signal handlers */
sig_atomic_t got_sighup = false;
sig_atomic_t got_sigterm = false;
sig_atomic_t got_sigusr1 = false;

/* GUC variables */
int	keeper_keepalives_time;
int	keeper_keepalives_count;
char *keeper_node_name;

/* Global variables */
KeeperStatus current_status;
int 		*PgKeeperPid;
KeeperNode 	*KeeperRepNodes;
int 		nKeeperRepNodes;
bool		promoted = false;

/*
 * add_node()
 *
 * Add specified node into table.
 */
Datum
add_node(PG_FUNCTION_ARGS)
{
	text *node_name = PG_GETARG_TEXT_P(0);
	text *conninfo = PG_GETARG_TEXT_P(1);
	char *standby_name;
	bool is_sync = false;
	bool is_master = false;
	Relation rel;
	TupleDesc tupdesc;
	int i;
	int num;

	/* Check connection with conninfo */
	if (!heartbeatServer(text_to_cstring(conninfo)))
	{
		ereport(WARNING, (errmsg("the server \"%s\", \"%s\" might not be available",
								 text_to_cstring(conninfo), text_to_cstring(conninfo))));
		PG_RETURN_BOOL(false);
	}

	parse_synchronous_standby_names();
	rel = get_rel_from_relname(cstring_to_text(KEEPER_MANAGE_TABLE_NAME), AccessShareLock,
							   ACL_SELECT);
	tupdesc = rel->rd_att;
	SetCurrentStatementStartTimestamp();
	SPI_connect();
	PushActiveSnapshot(GetTransactionSnapshot());

	/* If no tuple exists, this node will be inserted as a master */
	getAllRepNodes(&num, false);

	/* First register node mast be master */
	is_master = (num == 0);

	/* Consider this node should be inserted as a sync node */
	if (!is_master)
	{
		standby_name = RepConfig->member_names;
		for (i = 0; i < RepConfig->nmembers; i++)
		{
			if (pg_strcasecmp(standby_name, text_to_cstring(node_name)) == 0)
			{
				is_sync = true;
				break;
			}
			standby_name += strlen(standby_name) + 1;
		}
	}

	/* insert node as master or standby*/
	addNewNode(rel->rd_att, node_name, conninfo, is_master, false, is_sync);

	/* Update next mater among with nodes */
	updateManageTableAccordingToSSNames(false);

	/* update next master info */
	//CommitTransactionCommand();

	relation_close(rel, AccessShareLock);
	SPI_finish();
	PopActiveSnapshot();

	/* Inform keeper process to update its local cache */
	kill(*PgKeeperPid, SIGUSR1);

	PG_RETURN_BOOL(true);
}

/*
 * Delete node by node name.
 */
Datum
del_node(PG_FUNCTION_ARGS)
{
	char *node_name = text_to_cstring(PG_GETARG_TEXT_PP(0));
	bool ret;

	SetCurrentStatementStartTimestamp();
	SPI_connect();
	PushActiveSnapshot(GetTransactionSnapshot());

	ret = deleteNodeByName(node_name);

	updateManageTableAccordingToSSNames(false);

	SPI_finish();
	PopActiveSnapshot();

	/* Inform keeper process to update its local cache */
	kill(*PgKeeperPid, SIGUSR1);

	PG_RETURN_BOOL(ret);
}

/*
 * Delete node by seqno.
 */
Datum
del_node_by_seqno(PG_FUNCTION_ARGS)
{
	int seqno = PG_GETARG_INT32(0);
	bool ret;

	SetCurrentStatementStartTimestamp();
	SPI_connect();
	PushActiveSnapshot(GetTransactionSnapshot());

	ret = deleteNodeBySeqno(seqno);

	updateManageTableAccordingToSSNames(false);

	SPI_finish();
	PopActiveSnapshot();

	/* Inform keeper process to update its local cache */
	kill(*PgKeeperPid, SIGUSR1);

	PG_RETURN_BOOL(ret);
}

/*
 * Polling given server used for indirectly polling.
 */
Datum
indirect_polling(PG_FUNCTION_ARGS)
{
	text *conninfo = PG_GETARG_TEXT_PP(0);
	bool ret;

	ret = heartbeatServer(text_to_cstring(conninfo));

	PG_RETURN_BOOL(ret);
}

/*
 * Send SIGUSR1 signal to pg_keeper process.
 */
Datum
indirect_kill(PG_FUNCTION_ARGS)
{
	char *signal = text_to_cstring(PG_GETARG_TEXT_PP(0));
	int pid = *PgKeeperPid;
	int sig;

	if (pg_strcasecmp(signal, "SIGUSR1") == 0)
		sig = SIGUSR1;
	else if (pg_strcasecmp(signal, "SIGUSR1") == 0)
		sig = SIGUSR1;
	else
		ereport(ERROR, (errmsg("Invalid signal \"%s\"", signal)));

	kill(pid, sig);

	PG_RETURN_BOOL(true);
}

/*
 * Entrypoint of this module.
 *
 * We register more than one worker process here, to demonstrate how that can
 * be done.
 */
void
_PG_init(void)
{
	BackgroundWorker worker;

	if (!process_shared_preload_libraries_in_progress)
		return;

	/* get the configuration */
	DefineCustomIntVariable("pg_keeper.keepalives_time",
							"Specific time between polling to primary server",
							NULL,
							&keeper_keepalives_time,
							5,
							1,
							INT_MAX,
							PGC_SIGHUP,
							0,
							NULL,
							NULL,
							NULL);

	DefineCustomIntVariable("pg_keeper.keepalives_count",
							"Specific retry count until promoting standby server",
							NULL,
							&keeper_keepalives_count,
							1,
							1,
							INT_MAX,
							PGC_SIGHUP,
							0,
							NULL,
							NULL,
							NULL);

	DefineCustomStringVariable("pg_keeper.after_command",
							   "Shell command that will be called after promoted",
							   NULL,
							   &keeper_after_command,
							   NULL,
							   PGC_SIGHUP,
							   GUC_NOT_IN_SAMPLE,
							   NULL,
							   NULL,
							   NULL);

	DefineCustomStringVariable("pg_keeper.node_name",
							   "Node name used clustering management",
							   NULL,
							   &keeper_node_name,
							   NULL,
							   PGC_SIGHUP,
							   GUC_NOT_IN_SAMPLE,
							   NULL,
							   NULL,
							   NULL);

	/* set up common data for all our workers */
	worker.bgw_flags = BGWORKER_SHMEM_ACCESS |
		BGWORKER_BACKEND_DATABASE_CONNECTION;
	worker.bgw_start_time = BgWorkerStart_ConsistentState;
	worker.bgw_restart_time = BGW_NEVER_RESTART;
	worker.bgw_main = KeeperMain;
	worker.bgw_notify_pid = 0;

	/* Install hooks */
	prev_shmem_startup_hook = shmem_startup_hook;
	shmem_startup_hook = pg_keeper_shmem_startup;

	/*
	 * Now fill in worker-specific data, and do the actual registrations.
	 */
	snprintf(worker.bgw_name, BGW_MAXLEN, "pg_keeper");
	worker.bgw_main_arg = Int32GetDatum(1);
	RegisterBackgroundWorker(&worker);
}

/*
 * Signal handler for SIGTERM
 *		Set a flag to let the main loop to terminate, and set our latch to wake
 *		it up.
 */
static void
pg_keeper_sigterm(SIGNAL_ARGS)
{
	int			save_errno = errno;

	got_sigterm = true;

	if (MyProc)
		SetLatch(&MyProc->procLatch);

	errno = save_errno;
}

/*
 * Signal handler for SIGHUP
 *		Set a flag to let the main loop to reread the config file, and set
 *		our latch to wake it up.
 */
static void
pg_keeper_sighup(SIGNAL_ARGS)
{
	got_sighup = true;
	if (MyProc)
		SetLatch(&MyProc->procLatch);
}

/*
 * Singnal handler for SIGUSR1
 *		Set a flag to let the main loop to update its local cache
 */
static void
pg_keeper_sigusr1(SIGNAL_ARGS)
{
	got_sigusr1 = true;
	if (MyProc)
		SetLatch(&MyProc->procLatch);
}

/*
 * Hook for shmem startup.
 */
static void
pg_keeper_shmem_startup(void)
{
	bool found;
	int shmem_size;

	if (prev_shmem_startup_hook)
		prev_shmem_startup_hook();

	shmem_size = MAXALIGN(sizeof(int));

	LWLockAcquire(AddinShmemInitLock, LW_EXCLUSIVE);
	PgKeeperPid = ShmemInitStruct("pg_keeper",
								  shmem_size,
								  &found);
	LWLockRelease(AddinShmemInitLock);
}

/*
 * Entry point for pg_keeper.
 */
void
KeeperMain(Datum main_arg)
{
	int ret;

	/* Sanity check */
	checkParameter();

	/* Determine keeper mode of itself */
	current_status = RecoveryInProgress() ? KEEPER_STANDBY_READY : KEEPER_MASTER_READY;

	/* Establish signal handlers before unblocking signals */
	pqsignal(SIGHUP, pg_keeper_sighup);
	pqsignal(SIGTERM, pg_keeper_sigterm);
	pqsignal(SIGUSR1, pg_keeper_sigusr1);

	/* We're now ready to receive signals */
	BackgroundWorkerUnblockSignals();

	/* Connect to our database */
	BackgroundWorkerInitializeConnection("postgres", NULL);

	/* Register my processid to shmem */
	*PgKeeperPid = MyProcPid;

	/* Parse and fetch configuration for synchronous replication */
	parse_synchronous_standby_names();

exec:
	if (current_status == KEEPER_MASTER_READY)
	{
		/* Routine for master_mode */
		setupKeeperMaster();
		ret = KeeperMainMaster();
	}
	else if (current_status == KEEPER_STANDBY_READY)
	{

		/* Routine for standby_mode */
		setupKeeperStandby();
		ret = KeeperMainStandby();

		/*
		 * After promoting is sucessfully done, attempt to re-execute
		 * main routine as master mode in order to avoid to restart
		 * for invoking pg_keeper process again.
		 */
		if (ret)
		{
			/* Change mode to master mode */
			current_status = KEEPER_MASTER_READY;

			/*
			 * XXX : We should switch to master server correctly.
			 */

			goto exec;
		}
	}
	else
		ereport(ERROR, (errmsg("invalid keeper mode : \"%d\"", current_status)));

	proc_exit(ret);
}

/*
 * heartbeatServer()
 *
 * This fucntion does heartbeating to given server using HEARTBEAT_SQL.
 * If could not establish connection to server or server didn't reaction,
 * emits log message and return false.
 */
bool
heartbeatServer(const char *conninfo)
{
	if (!(execSQL(conninfo, HEARTBEAT_SQL, NULL)))
		return false;

	return true;
}

/*
 * Simple function to execute one SQL.
 */
bool
execSQL(const char *conninfo, const char *sql, bool *result)
{
	PGconn		*con;
	PGresult 	*res;

	/* Try to connect to primary server */
	if ((con = PQconnectdb(conninfo)) == NULL)
	{
		ereport(LOG,
				(errmsg("could not establish conenction to server : \"%s\"",
					conninfo)));

		PQfinish(con);
		return false;
	}

	res = PQexec(con, sql);

	if (PQresultStatus(res) != PGRES_TUPLES_OK &&
		PQresultStatus(res) != PGRES_COMMAND_OK)
	{
		/* Failed to ping to master server, report the number of retrying */
		ereport(LOG,
				(errmsg("could not get tuple from server : \"%s\"",
					conninfo)));

		PQfinish(con);
		return false;
	}

	if (result != NULL)
		*result = str_to_bool(PQgetvalue(res, 0, 0));

	/* Primary server is alive now */
	PQfinish(con);
	return true;
}

/* Check the mandatory parameteres */
static void
checkParameter()
{
	if (keeper_node_name == NULL || keeper_node_name[0] == '\0')
		elog(ERROR, "pg_keeper.node_name must be specified.");
}

/*
 * Return status string for process title.
 */
char *
getStatusPsString(KeeperStatus status, int num)
{
	StringInfoData str;

	initStringInfo(&str);

	if (status == KEEPER_STANDBY_READY)
		return "(standby:ready)";
	else if (status == KEEPER_STANDBY_CONNECTED)
		appendStringInfo(&str, "(standby:connected, %d)", num);
	else if (status == KEEPER_STANDBY_ALONE)
		appendStringInfo(&str, "(standby:alone, %d)", num);
	else if (status == KEEPER_MASTER_READY)
		appendStringInfo(&str, "(master:ready, %d)", num);
	else if (status == KEEPER_MASTER_CONNECTED)
		appendStringInfo(&str, "(master:connected, %d)", num);
	else /* status == KEEPER_MASTER_ASYNC) */
		appendStringInfo(&str, "(master:async, %d)", num);

	return str.data;
}
