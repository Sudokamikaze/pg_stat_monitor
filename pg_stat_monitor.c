/*-------------------------------------------------------------------------
 *
 * pg_stat_monitor.c
 *		Track statement execution times across a whole database cluster.
 *
 * Copyright (c) 2008-2018, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  contrib/pg_stat_monitor/pg_stat_monitor.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "pg_stat_monitor.h"

PG_MODULE_MAGIC;

/*---- Initicalization Function Declarations ----*/
void _PG_init(void);
void _PG_fini(void);


/*---- Local variables ----*/

/* Current nesting depth of ExecutorRun+ProcessUtility calls */
static int	nested_level = 0;
#if PG_VERSION_NUM >= 130000
static int	plan_nested_level = 0;
static int	exec_nested_level = 0;
#endif
static struct rusage  rusage_start;
static struct rusage  rusage_end;
static volatile sig_atomic_t sigterm = false;
static void handle_sigterm(SIGNAL_ARGS);

int query_buf_size_bucket;
HTAB *
CreateHash(const char *hash_name, int key_size, int entry_size, int hash_size);

/* Saved hook values in case of unload */
static planner_hook_type planner_hook_next = NULL;
static shmem_startup_hook_type prev_shmem_startup_hook = NULL;
static post_parse_analyze_hook_type prev_post_parse_analyze_hook = NULL;
static ExecutorStart_hook_type prev_ExecutorStart = NULL;
static ExecutorRun_hook_type prev_ExecutorRun = NULL;
static ExecutorFinish_hook_type prev_ExecutorFinish = NULL;
static ExecutorEnd_hook_type prev_ExecutorEnd = NULL;
static ProcessUtility_hook_type prev_ProcessUtility = NULL;

/* Links to shared memory state */
static pgssSharedState *pgss = NULL;
static HTAB *pgss_hash = NULL;
static HTAB *pgss_object_hash = NULL;

/* Hash table for aggegates */
static HTAB *pgss_agghash = NULL;

/* Hash table for aggegates */
static HTAB *pgss_buckethash = NULL;

/* Hash table for wait events */
static HTAB *pgss_waiteventshash = NULL;

static pgssBucketEntry **pgssBucketEntries = NULL;
static pgssWaitEventEntry **pgssWaitEventEntries = NULL;


PG_FUNCTION_INFO_V1(pg_stat_monitor_reset);
PG_FUNCTION_INFO_V1(pg_stat_monitor_1_2);
PG_FUNCTION_INFO_V1(pg_stat_monitor_1_3);
PG_FUNCTION_INFO_V1(pg_stat_monitor);
PG_FUNCTION_INFO_V1(pg_stat_wait_events);
PG_FUNCTION_INFO_V1(pg_stat_monitor_settings);

/* Extended version function prototypes */
PG_FUNCTION_INFO_V1(pg_stat_agg);
static uint pg_get_client_addr(void);
static Datum array_get_datum(int arr[]);

static void update_agg_counters(uint64 bucket_id, uint64 queryid, uint64 id, AGG_KEY type);
static pgssAggEntry *agg_entry_alloc(pgssAggHashKey *key);
void add_object_entry(uint64 queryid, char *objects);
#if PG_VERSION_NUM >= 130000
static PlannedStmt * pgss_planner_hook(Query *parse, const char *query_string, int cursorOptions, ParamListInfo boundParams);
#else
static void BufferUsageAccumDiff(BufferUsage* bufusage, BufferUsage* pgBufferUsage, BufferUsage* bufusage_start);
static PlannedStmt *pgss_planner_hook(Query *parse, int opt, ParamListInfo param);
#endif

static void pgss_shmem_startup(void);
static void pgss_shmem_shutdown(int code, Datum arg);
static void pgss_post_parse_analyze(ParseState *pstate, Query *query);
static void pgss_ExecutorStart(QueryDesc *queryDesc, int eflags);
static void pgss_ExecutorRun(QueryDesc *queryDesc, ScanDirection direction, uint64 count, bool execute_once);
static void pgss_ExecutorFinish(QueryDesc *queryDesc);
static void pgss_ExecutorEnd(QueryDesc *queryDesc);

#if PG_VERSION_NUM >= 130000
static void pgss_ProcessUtility(PlannedStmt *pstmt, const char *queryString,
					ProcessUtilityContext context,
					ParamListInfo params, QueryEnvironment *queryEnv,
					DestReceiver *dest,
					QueryCompletion *qc
					);
#else
static void pgss_ProcessUtility(PlannedStmt *pstmt, const char *queryString,
                                ProcessUtilityContext context, ParamListInfo params,
                                QueryEnvironment *queryEnv,
                                DestReceiver *dest,
                                char *completionTag);
#endif

static uint64 pgss_hash_string(const char *str, int len);
static void pgss_store(const char *query, uint64 queryId,
				int query_location, int query_len,
				bool kind,
				double total_time, uint64 rows,
				const BufferUsage *bufusage,
#if PG_VERSION_NUM >= 130000
				const WalUsage *walusage,
#endif
				pgssJumbleState *jstate,
				float utime, float stime);

static void pg_stat_monitor_internal(FunctionCallInfo fcinfo,
							bool showtext);
static Size pgss_memsize(void);
static pgssEntry *entry_alloc(pgssSharedState *pgss, pgssHashKey *key, Size query_offset, int query_len, int encoding, bool sticky);

static void entry_dealloc(int bucket_id);
static void entry_reset(void);
static void AppendJumble(pgssJumbleState *jstate,
			 const unsigned char *item, Size size);
static void JumbleQuery(pgssJumbleState *jstate, Query *query);
static void JumbleRangeTable(pgssJumbleState *jstate, List *rtable);
static void JumbleExpr(pgssJumbleState *jstate, Node *node);
static void RecordConstLocation(pgssJumbleState *jstate, int location);
static char *generate_normalized_query(pgssJumbleState *jstate, const char *query,
						  int query_loc, int *query_len_p, int encoding);
static void fill_in_constant_lengths(pgssJumbleState *jstate, const char *query,
						 int query_loc);
static int comp_location(const void *a, const void *b);

static uint64 get_next_wbucket(pgssSharedState *pgss);

static void store_query(uint64 queryid, const char *query, uint64 query_len);
static uint64 locate_query(uint64 bucket_id, uint64 queryid, char * query);

/* Wait Event Local Functions */
static void register_wait_event(void);
void wait_event_main(Datum main_arg);
static void update_wait_event(void);
static uint64 get_query_id(pgssJumbleState *jstate, Query *query);

/*
 * Module load callback
 */
void
_PG_init(void)
{
	elog(DEBUG2, "pg_stat_monitor: %s()", __FUNCTION__);
	/*
	 * In order to create our shared memory area, we have to be loaded via
	 * shared_preload_libraries.  If not, fall out without hooking into any of
	 * the main system.  (We don't throw error here because it seems useful to
	 * allow the pg_stat_statements functions to be created even when the
	 * module isn't active.  The functions must protect themselves against
	 * being called then, however.)
	 */
	if (!process_shared_preload_libraries_in_progress)
		return;

	/* Inilize the GUC variables */
	init_guc();

	EmitWarningsOnPlaceholders("pg_stat_monitor");

	/*
	 * Request additional shared resources.  (These are no-ops if we're not in
	 * the postmaster process.)  We'll allocate or attach to the shared
	 * resources in pgss_shmem_startup().
	 */
	RequestAddinShmemSpace(pgss_memsize());
	RequestNamedLWLockTranche("pg_stat_monitor", 1);

	/* Register Wait events */
	register_wait_event();

	/*
	 * Install hooks.
	 */
	prev_shmem_startup_hook 		= shmem_startup_hook;
	shmem_startup_hook 				= pgss_shmem_startup;
	prev_post_parse_analyze_hook 	= post_parse_analyze_hook;
	post_parse_analyze_hook 		= pgss_post_parse_analyze;
	prev_ExecutorStart 				= ExecutorStart_hook;
	ExecutorStart_hook 				= pgss_ExecutorStart;
	prev_ExecutorRun 				= ExecutorRun_hook;
	ExecutorRun_hook 				= pgss_ExecutorRun;
	prev_ExecutorFinish 			= ExecutorFinish_hook;
	ExecutorFinish_hook 			= pgss_ExecutorFinish;
	prev_ExecutorEnd 				= ExecutorEnd_hook;
	ExecutorEnd_hook 				= pgss_ExecutorEnd;
	prev_ProcessUtility 			= ProcessUtility_hook;
	ProcessUtility_hook 			= pgss_ProcessUtility;
	planner_hook_next       		= planner_hook;
	planner_hook            		= pgss_planner_hook;
}

/*
 * Module unload callback
 */
void
_PG_fini(void)
{
	elog(DEBUG2, "pg_stat_monitor: %s()", __FUNCTION__);

	shmem_startup_hook 		= prev_shmem_startup_hook;
	post_parse_analyze_hook = prev_post_parse_analyze_hook;
	ExecutorStart_hook 		= prev_ExecutorStart;
	ExecutorRun_hook 		= prev_ExecutorRun;
	ExecutorFinish_hook 	= prev_ExecutorFinish;
	ExecutorEnd_hook 		= prev_ExecutorEnd;
	ProcessUtility_hook 	= prev_ProcessUtility;
	entry_reset();
}

HTAB *
CreateHash(const char *hash_name, int key_size, int entry_size, int hash_size)
{
	HASHCTL info;
	memset(&info, 0, sizeof(info));
	info.keysize = key_size;
	info.entrysize = entry_size;
	return ShmemInitHash(hash_name, hash_size, hash_size, &info, HASH_ELEM | HASH_BLOBS);
}


/*
 * shmem_startup hook: allocate or attach to shared memory,
 * then load any pre-existing statistics from file.
 * Also create and load the query-texts file, which is expected to exist
 * (even if empty) while the module is enabled.
 */
static void
pgss_shmem_startup(void)
{
	bool		found = false;
	int32		i;

	if (prev_shmem_startup_hook)
		prev_shmem_startup_hook();

	/* reset in case this is a restart within the postmaster */
	pgss = NULL;
	pgss_hash = NULL;
	pgss_object_hash = NULL;
	pgss_agghash = NULL;
	pgss_buckethash = NULL;
	pgss_waiteventshash = NULL;

	/*
	 * Create or attach to the shared memory state, including hash table
	 */
	LWLockAcquire(AddinShmemInitLock, LW_EXCLUSIVE);

	pgss = ShmemInitStruct("pg_stat_monitor", sizeof(pgssSharedState), &found);
	if (!found)
	{
		/* First time through ... */
		pgss->lock = &(GetNamedLWLockTranche("pg_stat_monitor"))->lock;
		SpinLockInit(&pgss->mutex);
		ResetSharedState(pgss);
	}

	query_buf_size_bucket = PGSM_QUERY_BUF_SIZE / PGSM_MAX_BUCKETS;
	for (i = 0; i < PGSM_MAX_BUCKETS; i++)
		pgss_qbuf[i] = (unsigned char *) ShmemAlloc(query_buf_size_bucket);

	pgss_hash = CreateHash("pg_stat_monitor: Queries hashtable",
							sizeof(pgssHashKey),
							sizeof(pgssEntry),
							PGSM_MAX);

	pgss_buckethash = CreateHash("pg_stat_monitor: Bucket hashtable",
							sizeof(pgssBucketHashKey),
							sizeof(pgssBucketEntry),
							PGSM_MAX_BUCKETS);

	pgss_waiteventshash = CreateHash("pg_stat_monitor: Wait Event hashtable",
							sizeof(pgssWaitEventKey),
							sizeof(pgssWaitEventEntry),
							100);

	pgss_object_hash = CreateHash("pg_stat_monitor: Object hashtable",
							sizeof(pgssObjectHashKey),
							sizeof(pgssObjectEntry),
							PGSM_OBJECT_CACHE);

	pgss_agghash = CreateHash("pg_stat_monitor: Aggregate hashtable",
							sizeof(pgssAggHashKey),
							sizeof(pgssAggEntry),
							PGSM_MAX * 3);

	Assert(IsHashInitialize());

	pgssWaitEventEntries = malloc(sizeof (pgssWaitEventEntry) * MAX_BACKEND_PROCESES);
	for (i = 0; i < MAX_BACKEND_PROCESES; i++)
	{
		pgssWaitEventKey	key;
		pgssWaitEventEntry	*entry = NULL;
		bool				found = false;

		key.processid = i;
		entry = (pgssWaitEventEntry *) hash_search(pgss_waiteventshash, &key, HASH_ENTER, &found);
		if (!found)
		{
			SpinLockInit(&entry->mutex);
			pgssWaitEventEntries[i] = entry;
		}
	}

	pgssBucketEntries = malloc(sizeof (pgssBucketEntry) * PGSM_MAX_BUCKETS);
	for (i = 0; i < PGSM_MAX_BUCKETS; i++)
	{
		pgssBucketHashKey	key;
		pgssBucketEntry		*entry = NULL;
		bool				found = false;

		key.bucket_id = i;
		/* Find or create an entry with desired hash code */
		entry = (pgssBucketEntry *) hash_search(pgss_buckethash, &key, HASH_ENTER, &found);
		if (!found)
		{
			memset(&entry->counters, 0, sizeof(pgssBucketCounters));
			SpinLockInit(&entry->mutex);
			pgssBucketEntries[i] = entry;
		}
	}

	LWLockRelease(AddinShmemInitLock);

	/*
	 * If we're in the postmaster (or a standalone backend...), set up a shmem
	 * exit hook to dump the statistics to disk.
	 */
	if (!IsUnderPostmaster)
		on_shmem_exit(pgss_shmem_shutdown, (Datum) 0);
}

/*
 * shmem_shutdown hook: Dump statistics into file.
 *
 * Note: we don't bother with acquiring lock, because there should be no
 * other processes running when this is called.
 */
static void
pgss_shmem_shutdown(int code, Datum arg)
{
	elog(DEBUG2, "pg_stat_monitor: %s()", __FUNCTION__);
	/* Don't try to dump during a crash. */
	if (code)
		return;

	/* Safety check ... shouldn't get here unless shmem is set up. */
	if (IsHashInitialize())
		return;
}


/*
 * Post-parse-analysis hook: mark query with a queryId
 */
static void
pgss_post_parse_analyze(ParseState *pstate, Query *query)
{
	pgssJumbleState jstate;
	char			tables_name[MAX_REL_LEN] = {0};

	if (prev_post_parse_analyze_hook)
		prev_post_parse_analyze_hook(pstate, query);

	/* Assert we didn't do this already */
	Assert(query->queryId == UINT64CONST(0));

	/* Safety check... */
	if (!IsHashInitialize())
		return;

	/*
	 * Utility statements get queryId zero.  We do this even in cases where
	 * the statement contains an optimizable statement for which a queryId
	 * could be derived (such as EXPLAIN or DECLARE CURSOR).  For such cases,
	 * runtime control will first go through ProcessUtility and then the
	 * executor, and we don't want the executor hooks to do anything, since we
	 * are already measuring the statement's costs at the utility level.
	 */
	if (query->utilityStmt)
	{
		query->queryId = UINT64CONST(0);
		return;
	}

	query->queryId = get_query_id(&jstate, query);
	if (query->rtable)
	{
		ListCell *lc;
		bool first = true;
		foreach(lc, query->rtable)
		{
			RangeTblEntry *rte = lfirst_node(RangeTblEntry, lc);
			if (rte->rtekind == RTE_RELATION)
			{
				char *relname = get_rel_name(rte->relid);
				char *relspacename = get_namespace_name(get_rel_namespace(rte->relid));
				if (relname)
				{
					if (first)
					{
						if (relspacename)
							snprintf(tables_name, MAX_REL_LEN, "%s.%s", relspacename, relname);
						else
							snprintf(tables_name, MAX_REL_LEN, "%s", relname);
						first = false;
					}
					else
					{
						if (relspacename)
							snprintf(tables_name, MAX_REL_LEN, "%s,%s.%s", tables_name, relspacename, relname);
						else
							snprintf(tables_name, MAX_REL_LEN, "%s,%s", tables_name, relname);
					}
				}
			}
		}
		LWLockAcquire(pgss->lock, LW_EXCLUSIVE);
		add_object_entry(query->queryId, tables_name);
		LWLockRelease(pgss->lock);
	}

	/*
	 * If we are unlucky enough to get a hash of zero, use 1 instead, to
	 * prevent confusion with the utility-statement case.
	 */
	if (query->queryId == UINT64CONST(0))
		query->queryId = UINT64CONST(1);

	if (jstate.clocations_count > 0)
		pgss_store(pstate->p_sourcetext,
				   query->queryId,
				   query->stmt_location,
				   query->stmt_len,
				   PGSS_INVALID,
				   0,
				   0,
				   NULL,
#if PG_VERSION_NUM >= 130000
				   NULL,
#endif
				   &jstate,
				   0.0,
				   0.0);
}

/*
 * ExecutorStart hook: start up tracking if needed
 */
static void
pgss_ExecutorStart(QueryDesc *queryDesc, int eflags)
{
	getrusage(RUSAGE_SELF, &rusage_start);

	if (prev_ExecutorStart)
		prev_ExecutorStart(queryDesc, eflags);
	else
		standard_ExecutorStart(queryDesc, eflags);

	/*
	 * If query has queryId zero, don't track it.  This prevents double
	 * counting of optimizable statements that are directly contained in
	 * utility statements.
	 */
	if (PGSS_ENABLED() && queryDesc->plannedstmt->queryId != UINT64CONST(0))
	{
		/*
		 * Set up to track total elapsed time in ExecutorRun.  Make sure the
		 * space is allocated in the per-query context so it will go away at
		 * ExecutorEnd.
		 */
		if (queryDesc->totaltime == NULL)
		{
			MemoryContext oldcxt;

			oldcxt = MemoryContextSwitchTo(queryDesc->estate->es_query_cxt);
			queryDesc->totaltime = InstrAlloc(1, INSTRUMENT_ALL);
			MemoryContextSwitchTo(oldcxt);
		}
	}
}

/*
 * ExecutorRun hook: all we need do is track nesting depth
 */
static void
pgss_ExecutorRun(QueryDesc *queryDesc, ScanDirection direction, uint64 count,
				 bool execute_once)
{
	nested_level++;
	PG_TRY();
	{
		if (prev_ExecutorRun)
			prev_ExecutorRun(queryDesc, direction, count, execute_once);
		else
			standard_ExecutorRun(queryDesc, direction, count, execute_once);
		nested_level--;
	}
	PG_CATCH();
	{
		nested_level--;
		PG_RE_THROW();
	}
	PG_END_TRY();
}

/*
 * ExecutorFinish hook: all we need do is track nesting depth
 */
static void
pgss_ExecutorFinish(QueryDesc *queryDesc)
{
	nested_level++;
	PG_TRY();
	{
		if (prev_ExecutorFinish)
			prev_ExecutorFinish(queryDesc);
		else
			standard_ExecutorFinish(queryDesc);
		nested_level--;
	}
	PG_CATCH();
	{
		nested_level--;
		PG_RE_THROW();
	}
	PG_END_TRY();
}

/*
 * ExecutorEnd hook: store results if needed
 */
static void
pgss_ExecutorEnd(QueryDesc *queryDesc)
{
	float   utime;
	float   stime;
	uint64	queryId = queryDesc->plannedstmt->queryId;

	if (queryId != UINT64CONST(0) && queryDesc->totaltime && PGSS_ENABLED())
	{
		/*
		 * Make sure stats accumulation is done.  (Note: it's okay if several
		 * levels of hook all do this.)
		 */
		InstrEndLoop(queryDesc->totaltime);
		getrusage(RUSAGE_SELF, &rusage_end);
		utime = TIMEVAL_DIFF(rusage_start.ru_utime, rusage_end.ru_utime);
		stime = TIMEVAL_DIFF(rusage_start.ru_stime, rusage_end.ru_stime);

		pgss_store(queryDesc->sourceText,
				   queryId,
				   queryDesc->plannedstmt->stmt_location,
				   queryDesc->plannedstmt->stmt_len,
				   PGSS_EXEC,
				   queryDesc->totaltime->total * 1000.0,	/* convert to msec */
				   queryDesc->estate->es_processed,
				   &queryDesc->totaltime->bufusage,
#if PG_VERSION_NUM >= 130000
				   &queryDesc->totaltime->walusage,
#endif
				   NULL,
				   utime,
				   stime);
	}

	if (prev_ExecutorEnd)
		prev_ExecutorEnd(queryDesc);
	else
		standard_ExecutorEnd(queryDesc);

}

/*
 * ProcessUtility hook
 */
#if PG_VERSION_NUM >= 130000
static void pgss_ProcessUtility(PlannedStmt *pstmt, const char *queryString,
								ProcessUtilityContext context,
								ParamListInfo params, QueryEnvironment *queryEnv,
								DestReceiver *dest,
								QueryCompletion *qc)
#else
static void pgss_ProcessUtility(PlannedStmt *pstmt, const char *queryString,
                                ProcessUtilityContext context, ParamListInfo params,
                                QueryEnvironment *queryEnv,
                                DestReceiver *dest,
                                char *completionTag)
#endif
{
	Node	   *parsetree = pstmt->utilityStmt;

	/*
	 * If it's an EXECUTE statement, we don't track it and don't increment the
	 * nesting level.  This allows the cycles to be charged to the underlying
	 * PREPARE instead (by the Executor hooks), which is much more useful.
	 *
	 * We also don't track execution of PREPARE.  If we did, we would get one
	 * hash table entry for the PREPARE (with hash calculated from the query
	 * string), and then a different one with the same query string (but hash
	 * calculated from the query tree) would be used to accumulate costs of
	 * ensuing EXECUTEs.  This would be confusing, and inconsistent with other
	 * cases where planning time is not included at all.
	 *
	 * Likewise, we don't track execution of DEALLOCATE.
	 */
	if (PGSM_TRACK_UTILITY &&
		!IsA(parsetree, ExecuteStmt) &&
		!IsA(parsetree, PrepareStmt) &&
		!IsA(parsetree, DeallocateStmt))
	{
		instr_time	start;
		instr_time	duration;
		uint64		rows;
		BufferUsage bufusage_start,
					bufusage;
#if PG_VERSION_NUM >= 130000
		WalUsage	walusage_start,
					walusage;
		walusage_start = pgWalUsage;
		exec_nested_level++;
#endif

		bufusage_start = pgBufferUsage;
		INSTR_TIME_SET_CURRENT(start);

		PG_TRY();
		{
			if (prev_ProcessUtility)
				prev_ProcessUtility(pstmt, queryString,
									context, params, queryEnv,
									dest
#if PG_VERSION_NUM >= 130000
									,qc
#else
									,completionTag
#endif
									);
			else
				standard_ProcessUtility(pstmt, queryString,
										context, params, queryEnv,
										dest
#if PG_VERSION_NUM >= 130000
									,qc
#else
									,completionTag
#endif
									);
		}
#if PG_VERSION_NUM >= 130000
		PG_FINALLY();
		{
			exec_nested_level--;
		}
#else
		PG_CATCH();
        {
			nested_level--;
			PG_RE_THROW();

		}
#endif
		PG_END_TRY();
		INSTR_TIME_SET_CURRENT(duration);
		INSTR_TIME_SUBTRACT(duration, start);

#if PG_VERSION_NUM >= 130000
		rows = (qc && qc->commandTag == CMDTAG_COPY) ? qc->nprocessed : 0;
		/* calc differences of WAL counters. */
		memset(&walusage, 0, sizeof(WalUsage));
		WalUsageAccumDiff(&walusage, &pgWalUsage, &walusage_start);
#else
		/* parse command tag to retrieve the number of affected rows. */
		if (completionTag && strncmp(completionTag, "COPY ", 5) == 0)
			rows = pg_strtouint64(completionTag + 5, NULL, 10);
		else
			rows = 0;
#endif

		/* calc differences of buffer counters. */
		memset(&bufusage, 0, sizeof(BufferUsage));
		BufferUsageAccumDiff(&bufusage, &pgBufferUsage, &bufusage_start);

		pgss_store(queryString,
				   0,			/* signal that it's a utility stmt */
				   pstmt->stmt_location,
				   pstmt->stmt_len,
				   PGSS_EXEC,
				   INSTR_TIME_GET_MILLISEC(duration),
				   rows,
				   &bufusage,
#if PG_VERSION_NUM >= 130000
				   &walusage,
#endif
				   NULL,
				   0,
				   0);
	}
	else
	{
			if (prev_ProcessUtility)
				prev_ProcessUtility(pstmt, queryString,
									context, params, queryEnv,
									dest
#if PG_VERSION_NUM >= 130000
									,qc
#else
									,completionTag
#endif
									);
				standard_ProcessUtility(pstmt, queryString,
										context, params, queryEnv,
										dest
#if PG_VERSION_NUM >= 130000
									,qc
#else
									,completionTag
#endif
									);
	}
}

#if PG_VERSION_NUM < 130000
static void
BufferUsageAccumDiff(BufferUsage* bufusage, BufferUsage* pgBufferUsage, BufferUsage* bufusage_start)
{
	/* calc differences of buffer counters. */
	bufusage->shared_blks_hit = pgBufferUsage->shared_blks_hit - bufusage_start->shared_blks_hit;
	bufusage->shared_blks_read = pgBufferUsage->shared_blks_read - bufusage_start->shared_blks_read;
	bufusage->shared_blks_dirtied = pgBufferUsage->shared_blks_dirtied - bufusage_start->shared_blks_dirtied;
	bufusage->shared_blks_written = pgBufferUsage->shared_blks_written - bufusage_start->shared_blks_written;
    bufusage->local_blks_hit = pgBufferUsage->local_blks_hit - bufusage_start->local_blks_hit;
	bufusage->local_blks_read = pgBufferUsage->local_blks_read - bufusage_start->local_blks_read;
	bufusage->local_blks_dirtied = pgBufferUsage->local_blks_dirtied - bufusage_start->local_blks_dirtied;
	bufusage->local_blks_written = pgBufferUsage->local_blks_written - bufusage_start->local_blks_written;
	bufusage->temp_blks_read = pgBufferUsage->temp_blks_read - bufusage_start->temp_blks_read;
	bufusage->temp_blks_written = pgBufferUsage->temp_blks_written - bufusage_start->temp_blks_written;
	bufusage->blk_read_time = pgBufferUsage->blk_read_time;
	INSTR_TIME_SUBTRACT(bufusage->blk_read_time, bufusage_start->blk_read_time);
	bufusage->blk_write_time = pgBufferUsage->blk_write_time;
	INSTR_TIME_SUBTRACT(bufusage->blk_write_time, bufusage_start->blk_write_time);
}
#endif
/*
 * Given an arbitrarily long query string, produce a hash for the purposes of
 * identifying the query, without normalizing constants.  Used when hashing
 * utility statements.
 */
static uint64
pgss_hash_string(const char *str, int len)
{
	return DatumGetUInt64(hash_any_extended((const unsigned char *) str,
											len, 0));
}

static uint
pg_get_client_addr(void)
{
	char	remote_host[NI_MAXHOST];
	int		num_backends = pgstat_fetch_stat_numbackends();
	int		ret;
	int		i;

	memset(remote_host, 0x0, NI_MAXHOST);
	for (i = 1; i <= num_backends; i++)
	{
		LocalPgBackendStatus *local_beentry;
		PgBackendStatus *beentry;

		local_beentry = pgstat_fetch_stat_local_beentry(i);
		beentry = &local_beentry->backendStatus;

		if (beentry->st_procpid == MyProcPid)
		{
			ret = pg_getnameinfo_all(&beentry->st_clientaddr.addr,
							 beentry->st_clientaddr.salen,
							 remote_host, sizeof(remote_host),
							 NULL, 0,
							 NI_NUMERICHOST | NI_NUMERICSERV);
			if (ret == 0)
				break;
			else
				return ntohl(inet_addr("127.0.0.1"));
		}
	}
	if (strcmp(remote_host, "[local]") == 0)
		return ntohl(inet_addr("127.0.0.1"));
	return ntohl(inet_addr(remote_host));
}


/*
 * Store some statistics for a statement.
 *
 * If queryId is 0 then this is a utility statement and we should compute
 * a suitable queryId internally.
 *
 * If jstate is not NULL then we're trying to create an entry for which
 * we have no statistics as yet; we just want to record the normalized
 * query string.  total_time, rows, bufusage are ignored in this case.
 */
static void pgss_store(const char *query, uint64 queryId,
				int query_location, int query_len,
				bool kind,
				double total_time, uint64 rows,
				const BufferUsage *bufusage,
#if PG_VERSION_NUM >= 130000
				const WalUsage *walusage,
#endif
				pgssJumbleState *jstate,
				float utime, float stime)
{
	pgssHashKey		key;
	pgssEntry		*entry;
	char			*norm_query = NULL;
	int				encoding = GetDatabaseEncoding();
	bool			reset = false;
	int				i;
	char			tables_name[MAX_REL_LEN] = {0};

	Assert(query != NULL);

	/* Safety check... */
	if (!IsHashInitialize() || !pgss_qbuf[pgss->current_wbucket])
		return;

	/*
	 * Confine our attention to the relevant part of the string, if the query
	 * is a portion of a multi-statement source string.
	 *
	 * First apply starting offset, unless it's -1 (unknown).
	 */
	if (query_location >= 0)
	{
		Assert(query_location <= strlen(query));
		query += query_location;
		/* Length of 0 (or -1) means "rest of string" */
		if (query_len <= 0)
			query_len = strlen(query);
		else
			Assert(query_len <= strlen(query));
	}
	else
	{
		/* If query location is unknown, distrust query_len as well */
		query_location = 0;
		query_len = strlen(query);
	}

	/*
	 * Discard leading and trailing whitespace, too.  Use scanner_isspace()
	 * not libc's isspace(), because we want to match the lexer's behavior.
	 */
	while (query_len > 0 && scanner_isspace(query[0]))
		query++, query_location++, query_len--;
	while (query_len > 0 && scanner_isspace(query[query_len - 1]))
		query_len--;

	/*
	 * For utility statements, we just hash the query string to get an ID.
	 */
	if (queryId == UINT64CONST(0))
		queryId = pgss_hash_string(query, query_len);


	{
		pgssObjectHashKey		key;
		pgssObjectEntry			*entry;

		key.queryid = queryId;

		LWLockAcquire(pgss->lock, LW_SHARED);
		entry = (pgssObjectEntry *) hash_search(pgss_object_hash, &key, HASH_FIND, NULL);
		if (entry != NULL)
		{
			LWLockRelease(pgss->lock);
			LWLockAcquire(pgss->lock, LW_EXCLUSIVE);
			snprintf(tables_name, MAX_REL_LEN, "%s", entry->tables_name);
			hash_search(pgss_object_hash, &entry->key, HASH_REMOVE, NULL);
		}
		LWLockRelease(pgss->lock);
	}

	/* Set up key for hashtable search */
	key.userid = GetUserId();
	key.dbid = MyDatabaseId;
	key.queryid = queryId;
	key.bucket_id = get_next_wbucket(pgss);

	if (key.bucket_id != pgss->current_wbucket)
	{
		reset = true;
		pgss->current_wbucket = key.bucket_id;
	}

	/* Lookup the hash table entry with shared lock. */
	LWLockAcquire(pgss->lock, LW_SHARED);
	entry = (pgssEntry *) hash_search(pgss_hash, &key, HASH_FIND, NULL);
	if(!entry)
	{
		/*
		 * Create a new, normalized query string if caller asked.  We don't
		 * need to hold the lock while doing this work.  (Note: in any case,
		 * it's possible that someone else creates a duplicate hashtable entry
		 * in the interval where we don't hold the lock below.  That case is
		 * handled by entry_alloc.)
		 */
		if (jstate)
		{
			LWLockRelease(pgss->lock);
			norm_query = generate_normalized_query(jstate, query,
												   query_location,
												   &query_len,
												   encoding);
			LWLockAcquire(pgss->lock, LW_SHARED);
		}

		LWLockRelease(pgss->lock);
		LWLockAcquire(pgss->lock, LW_EXCLUSIVE);

		/* OK to create a new hashtable entry */
		entry = entry_alloc(pgss, &key, 0, query_len, encoding, jstate != NULL);
		if (entry == NULL)
			goto exit;
	}

	if (PGSM_NORMALIZED_QUERY)
		store_query(queryId, norm_query ? norm_query : query, query_len);
	else
		store_query(queryId, query, query_len);

	/*
	 * Grab the spinlock while updating the counters (see comment about
	 * locking rules at the head of the file)
	 */
	{
		volatile pgssEntry *e = (volatile pgssEntry *) entry;
		/* Increment the counts, except when jstate is not NULL */
		if (!jstate)
		{

			SpinLockAcquire(&e->mutex);

			/* Start collecting data for next bucket and reset all counters */
			if (reset)
				memset(&entry->counters, 0, sizeof(Counters));

			/* Calculate the agregates for database/user and host */
			update_agg_counters(entry->key.bucket_id, key.queryid, key.dbid, AGG_KEY_DATABASE);
			update_agg_counters(entry->key.bucket_id, key.queryid, key.userid, AGG_KEY_USER);
			update_agg_counters(entry->key.bucket_id, key.queryid, pg_get_client_addr(), AGG_KEY_HOST);

		/* "Unstick" entry if it was previously sticky */
		if (e->counters.calls[kind].calls == 0)
			e->counters.calls[kind].usage = USAGE_INIT;
		e->counters.calls[kind].calls += 1;
		e->counters.time[kind].total_time += total_time;

		if (e->counters.calls[kind].calls == 1)
		{
			e->counters.time[kind].min_time = total_time;
			e->counters.time[kind].max_time = total_time;
			e->counters.time[kind].mean_time = total_time;
		}
		else
		{
			/*
			 * Welford's method for accurately computing variance. See
			 * <http://www.johndcook.com/blog/standard_deviation/>
			 */
			double old_mean = e->counters.time[kind].mean_time;

			e->counters.time[kind].mean_time +=
				(total_time - old_mean) / e->counters.calls[kind].calls;
			e->counters.time[kind].sum_var_time +=
				(total_time - old_mean) * (total_time - e->counters.time[kind].mean_time);

			/* calculate min and max time */
			if (e->counters.time[kind].min_time > total_time)
				e->counters.time[kind].min_time = total_time;
			if (e->counters.time[kind].max_time < total_time)
				e->counters.time[kind].max_time = total_time;
		}

		for (i = 0; i < MAX_RESPONSE_BUCKET - 1; i++)
		{
			if (total_time < PGSM_RESPOSE_TIME_LOWER_BOUND + (PGSM_RESPOSE_TIME_STEP * i))
			{
				pgssBucketEntries[entry->key.bucket_id]->counters.resp_calls[i]++;
				break;
			}
		}
		if (total_time > PGSM_RESPOSE_TIME_LOWER_BOUND + (PGSM_RESPOSE_TIME_STEP * MAX_RESPONSE_BUCKET))
			pgssBucketEntries[entry->key.bucket_id]->counters.resp_calls[MAX_RESPONSE_BUCKET - 1]++;

		e->counters.calls[kind].rows += rows;
		e->counters.blocks.shared_blks_hit += bufusage->shared_blks_hit;
		e->counters.blocks.shared_blks_read += bufusage->shared_blks_read;
		e->counters.blocks.shared_blks_dirtied += bufusage->shared_blks_dirtied;
		e->counters.blocks.shared_blks_written += bufusage->shared_blks_written;
		e->counters.blocks.local_blks_hit += bufusage->local_blks_hit;
		e->counters.blocks.local_blks_read += bufusage->local_blks_read;
		e->counters.blocks.local_blks_dirtied += bufusage->local_blks_dirtied;
		e->counters.blocks.local_blks_written += bufusage->local_blks_written;
		e->counters.blocks.temp_blks_read += bufusage->temp_blks_read;
		e->counters.blocks.temp_blks_written += bufusage->temp_blks_written;
		e->counters.blocks.blk_read_time += INSTR_TIME_GET_MILLISEC(bufusage->blk_read_time);
		e->counters.blocks.blk_write_time += INSTR_TIME_GET_MILLISEC(bufusage->blk_write_time);
		e->counters.calls[kind].usage += USAGE_EXEC(total_time);
		e->counters.info.host = pg_get_client_addr();
		e->counters.sysinfo.utime = utime;
		e->counters.sysinfo.stime = stime;
		for(i = 0; i < MAX_REL_LEN - 1; i++)
			e->counters.info.tables_name[i] = tables_name[i];
		SpinLockRelease(&e->mutex);
		}
	}

exit:
	LWLockRelease(pgss->lock);

	/* We postpone this clean-up until we're out of the lock */
	if (norm_query)
		pfree(norm_query);
}

/*
 * Reset all statement statistics.
 */
Datum
pg_stat_monitor_reset(PG_FUNCTION_ARGS)
{
	if (!pgss || !pgss_hash || !pgss_agghash || !pgss_buckethash || !pgss_waiteventshash)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("pg_stat_monitor: must be loaded via shared_preload_libraries")));
	entry_dealloc(-1);
	PG_RETURN_VOID();
}

#define PG_STAT_STATEMENTS_COLS         38  /* maximum of above */

Datum
pg_stat_wait_events(PG_FUNCTION_ARGS)
{
	ReturnSetInfo	*rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	TupleDesc		tupdesc;
	Tuplestorestate *tupstore;
	MemoryContext	per_query_ctx;
	MemoryContext	oldcontext;
	HASH_SEQ_STATUS hash_seq;
	pgssWaitEventEntry		*entry;
	char			*query_txt;
	char			queryid_txt[64];
	query_txt = (char*) malloc(PGSM_QUERY_MAX_LEN);

	/* hash table must exist already */
	if (!pgss || !pgss_hash || !pgss_object_hash)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("pg_stat_monitor: must be loaded via shared_preload_libraries")));

	/* check to see if caller supports us returning a tuplestore */
	if (rsinfo == NULL || !IsA(rsinfo, ReturnSetInfo))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("pg_stat_monitor: set-valued function called in context that cannot accept a set")));
	if (!(rsinfo->allowedModes & SFRM_Materialize))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("pg_stat_monitor: materialize mode required, but it is not " \
						"allowed in this context")));

	/* Switch into long-lived context to construct returned data structures */
	per_query_ctx = rsinfo->econtext->ecxt_per_query_memory;
	oldcontext = MemoryContextSwitchTo(per_query_ctx);

	/* Build a tuple descriptor for our result type */
	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "pg_stat_monitor: return type must be a row type");

	tupstore = tuplestore_begin_heap(true, false, work_mem);
	rsinfo->returnMode = SFRM_Materialize;
	rsinfo->setResult = tupstore;
	rsinfo->setDesc = tupdesc;

	MemoryContextSwitchTo(oldcontext);

	LWLockAcquire(pgss->lock, LW_SHARED);

	hash_seq_init(&hash_seq, pgss_waiteventshash);
	while ((entry = hash_seq_search(&hash_seq)) != NULL)
	{
		Datum		values[4];
		bool		nulls[4] = {true};
		int			i = 0;
		int64		queryid = entry->key.queryid;

		if (queryid == 0)
				continue;
		memset(values, 0, sizeof(values));
		memset(nulls, 0, sizeof(nulls));

		sprintf(queryid_txt, "%08lX", queryid);

		values[i++] = ObjectIdGetDatum(cstring_to_text(queryid_txt));
		values[i++] = ObjectIdGetDatum(entry->pid);
		if (entry->wait_event_info != 0)
		{
			const char *event_type = pgstat_get_wait_event_type(entry->wait_event_info);
			const char *event = pgstat_get_wait_event(entry->wait_event_info);
			if (event_type)
				values[i++] = PointerGetDatum(cstring_to_text(event_type));
			else
				nulls[i++] = true;
			if (event)
				values[i++] = PointerGetDatum(cstring_to_text(event));
			else
				nulls[i++] = true;
		}
		else
		{
			nulls[i++] = true;
			nulls[i++] = true;
		}
		tuplestore_putvalues(tupstore, tupdesc, values, nulls);
	}
	free(query_txt);

	/* clean up and return the tuplestore */
	LWLockRelease(pgss->lock);

	tuplestore_donestoring(tupstore);
	return (Datum) 0;
}


Datum
pg_stat_monitor(PG_FUNCTION_ARGS)
{
	/* If it's really API 1.1, we'll figure that out below */
	pg_stat_monitor_internal(fcinfo, true);
	return (Datum) 0;
}

/* Common code for all versions of pg_stat_statements() */
static void
pg_stat_monitor_internal(FunctionCallInfo fcinfo,
						bool showtext)
{
	ReturnSetInfo	*rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	TupleDesc		tupdesc;
	Tuplestorestate *tupstore;
	MemoryContext	per_query_ctx;
	MemoryContext	oldcontext;
	Oid				userid = GetUserId();
	bool			is_allowed_role = false;
	HASH_SEQ_STATUS hash_seq;
	pgssEntry		*entry;
	char			*query_txt;
	char			queryid_txt[64];
	query_txt = (char*) malloc(PGSM_QUERY_MAX_LEN);

	/* Superusers or members of pg_read_all_stats members are allowed */
	is_allowed_role = is_member_of_role(GetUserId(), DEFAULT_ROLE_READ_ALL_STATS);

	/* hash table must exist already */
	if (!pgss || !pgss_hash || !pgss_object_hash)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("pg_stat_monitor: must be loaded via shared_preload_libraries")));

	/* check to see if caller supports us returning a tuplestore */
	if (rsinfo == NULL || !IsA(rsinfo, ReturnSetInfo))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("pg_stat_monitor: set-valued function called in context that cannot accept a set")));
	if (!(rsinfo->allowedModes & SFRM_Materialize))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("pg_stat_monitor: materialize mode required, but it is not " \
						"allowed in this context")));

	/* Switch into long-lived context to construct returned data structures */
	per_query_ctx = rsinfo->econtext->ecxt_per_query_memory;
	oldcontext = MemoryContextSwitchTo(per_query_ctx);

	/* Build a tuple descriptor for our result type */
	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "pg_stat_monitor: return type must be a row type");

	tupstore = tuplestore_begin_heap(true, false, work_mem);
	rsinfo->returnMode = SFRM_Materialize;
	rsinfo->setResult = tupstore;
	rsinfo->setDesc = tupdesc;

	MemoryContextSwitchTo(oldcontext);

	LWLockAcquire(pgss->lock, LW_SHARED);

	hash_seq_init(&hash_seq, pgss_hash);
	while ((entry = hash_seq_search(&hash_seq)) != NULL)
	{
		Datum		values[PG_STAT_STATEMENTS_COLS];
		bool		nulls[PG_STAT_STATEMENTS_COLS];
		int			i = 0;
		Counters	tmp;
		double		stddev;
		int64		queryid = entry->key.queryid;

		memset(values, 0, sizeof(values));
		memset(nulls, 0, sizeof(nulls));

		if(locate_query(entry->key.bucket_id, queryid, query_txt) == 0)
			sprintf(query_txt, "%s", "<invalid query text, probably no space left in shared buffer>");

		sprintf(queryid_txt, "%08lX", queryid);

		values[i++] = ObjectIdGetDatum(entry->key.bucket_id);
		values[i++] = ObjectIdGetDatum(entry->key.userid);
		values[i++] = ObjectIdGetDatum(entry->key.dbid);
		/* copy counters to a local variable to keep locking time short */
		{
			volatile pgssEntry *e = (volatile pgssEntry *) entry;
			SpinLockAcquire(&e->mutex);
			tmp = e->counters;
			SpinLockRelease(&e->mutex);
		}
		if (is_allowed_role || entry->key.userid == userid)
		{
			values[i++] = CStringGetTextDatum(queryid_txt);
			if (showtext)
			{
					char	*enc;
					enc = pg_any_to_server(query_txt, strlen(query_txt), entry->encoding);
					values[i++] = CStringGetTextDatum(enc);
					if (enc != query_txt)
						pfree(enc);
			}
		    else
			{
				/* Query text not requested */
				nulls[i++] = true;
			}
		}
		else
		{
			/*
			 * Don't show query text, but hint as to the reason for not doing
			 *	so if it was requested
			 */
			if (showtext)
				values[i++] = CStringGetTextDatum("<insufficient privilege>");
			else
				nulls[i++] = true;
		}

		values[i++] = TimestampGetDatum(pgssBucketEntries[entry->key.bucket_id]->counters.current_time);

		for (int kind = 0; kind < PGSS_NUMKIND; kind++)
		{
			values[i++] = Int64GetDatumFast(tmp.calls[kind].calls);
			values[i++] = Float8GetDatumFast(tmp.time[kind].total_time);
			values[i++] = Float8GetDatumFast(tmp.time[kind].min_time);
			values[i++] = Float8GetDatumFast(tmp.time[kind].max_time);
			values[i++] = Float8GetDatumFast(tmp.time[kind].mean_time);
			if (tmp.calls[kind].calls > 1)
				stddev = sqrt(tmp.time[kind].sum_var_time / tmp.calls[kind].calls);
			else
				stddev = 0.0;
			values[i++] = Float8GetDatumFast(stddev);
			values[i++] = Int64GetDatumFast(tmp.calls[kind].rows);
		}
		values[i++] = Int64GetDatumFast(tmp.blocks.shared_blks_hit);
		values[i++] = Int64GetDatumFast(tmp.blocks.shared_blks_read);
		values[i++] = Int64GetDatumFast(tmp.blocks.shared_blks_dirtied);
		values[i++] = Int64GetDatumFast(tmp.blocks.shared_blks_written);
		values[i++] = Int64GetDatumFast(tmp.blocks.local_blks_hit);
		values[i++] = Int64GetDatumFast(tmp.blocks.local_blks_read);
		values[i++] = Int64GetDatumFast(tmp.blocks.local_blks_dirtied);
		values[i++] = Int64GetDatumFast(tmp.blocks.local_blks_written);
		values[i++] = Int64GetDatumFast(tmp.blocks.temp_blks_read);
		values[i++] = Int64GetDatumFast(tmp.blocks.temp_blks_written);
		values[i++] = Float8GetDatumFast(tmp.blocks.blk_read_time);
		values[i++] = Float8GetDatumFast(tmp.blocks.blk_write_time);
		values[i++] = Int64GetDatumFast(tmp.info.host);
		values[i++] = ArrayGetTextDatum(pgssBucketEntries[entry->key.bucket_id]->counters.resp_calls);
		values[i++] = Float8GetDatumFast(tmp.sysinfo.utime);
		values[i++] = Float8GetDatumFast(tmp.sysinfo.stime);
		if (strlen(tmp.info.tables_name) == 0)
			nulls[i++] = true;
		else
			values[i++] = CStringGetTextDatum(tmp.info.tables_name);
		tuplestore_putvalues(tupstore, tupdesc, values, nulls);
	}
	free(query_txt);

	/* clean up and return the tuplestore */
	LWLockRelease(pgss->lock);

	tuplestore_donestoring(tupstore);
}

/*
 * Estimate shared memory space needed.
 */
static Size
pgss_memsize(void)
{
	Size	size;

	size = MAXALIGN(sizeof(pgssSharedState));
	size = add_size(size, hash_estimate_size(PGSM_MAX, sizeof(pgssEntry)));

	return size;
}

/*
 * Allocate a new hashtable entry.
 * caller must hold an exclusive lock on pgss->lock
 *
 * "query" need not be null-terminated; we rely on query_len instead
 *
 * If "sticky" is true, make the new entry artificially sticky so that it will
 * probably still be there when the query finishes execution.  We do this by
 * giving it a median usage value rather than the normal value.  (Strictly
 * speaking, query strings are normalized on a best effort basis, though it
 * would be difficult to demonstrate this even under artificial conditions.)
 *
 * Note: despite needing exclusive lock, it's not an error for the target
 * entry to already exist.  This is because pgss_store releases and
 * reacquires lock after failing to find a match; so someone else could
 * have made the entry while we waited to get exclusive lock.
 */
static pgssEntry *
entry_alloc(pgssSharedState *pgss, pgssHashKey *key, Size query_offset, int query_len, int encoding,
			bool sticky)
{
	pgssEntry	*entry = NULL;
	bool		found = false;

	if (pgss->bucket_entry[pgss->current_wbucket] >= (PGSM_MAX / PGSM_MAX_BUCKETS))
	{
		pgss->bucket_overflow[pgss->current_wbucket]++;
		return NULL;
	}

	if (hash_get_num_entries(pgss_hash)  >= PGSM_MAX)
		return NULL;

	/* Find or create an entry with desired hash code */
	entry = (pgssEntry *) hash_search(pgss_hash, key, HASH_ENTER, &found);
	if (!found)
	{
		pgss->bucket_entry[pgss->current_wbucket]++;
		/* New entry, initialize it */

		/* reset the statistics */
		memset(&entry->counters, 0, sizeof(Counters));
		/* set the appropriate initial usage count */
		entry->counters.calls[0].usage = sticky ? pgss->cur_median_usage : USAGE_INIT;
		/* re-initialize the mutex each time ... we assume no one using it */
		SpinLockInit(&entry->mutex);
		/* ... and don't forget the query text metadata */
		entry->encoding = encoding;
	}
	return entry;
}

static uint64
get_next_wbucket(pgssSharedState *pgss)
{
	struct timeval	tv;
	uint64	current_usec;
	uint64			bucket_id;

	gettimeofday(&tv,NULL);
	current_usec = tv.tv_sec;

	if ((current_usec - pgss->prev_bucket_usec) > PGSM_BUCKET_TIME)
	{
		bucket_id = pgss->current_wbucket + 1;
		if (bucket_id == PGSM_MAX_BUCKETS)
			bucket_id = 0;

		LWLockAcquire(pgss->lock, LW_EXCLUSIVE);
		entry_dealloc(bucket_id);
		/* reset the query buffer */
		pgss->query_fifo[bucket_id].head = 0;
		pgss->query_fifo[bucket_id].tail = 0;
		LWLockRelease(pgss->lock);

		pgss->prev_bucket_usec = current_usec;

		pgssBucketEntries[bucket_id]->counters.current_time = GetCurrentTimestamp();
		return bucket_id;
	}
	return pgss->current_wbucket;
}

/*
 * Deallocate least-used entries.
 *
 * Caller must hold an exclusive lock on pgss->lock.
 */
static void
entry_dealloc(int bucket)
{
	HASH_SEQ_STATUS hash_seq;
	HASH_SEQ_STATUS hash_dbseq;
	pgssEntry		*entry;
	pgssAggEntry	*agg_entry;
	pgssEntry		**entries;
	pgssAggEntry	**agg_entries;
	int				i;
	int				nvictims = 0;

	pgss->bucket_entry[bucket] = 0;

	entries = palloc(hash_get_num_entries(pgss_hash) * sizeof(pgssEntry *));
	hash_seq_init(&hash_seq, pgss_hash);
	while ((entry = hash_seq_search(&hash_seq)) != NULL)
	{
		if (entry->key.bucket_id == bucket || bucket < 0)
			entries[nvictims++] = entry;
	}

	for (i = 0; i < nvictims; i++)
		entry = hash_search(pgss_hash, &entries[i]->key, HASH_REMOVE, NULL);

	nvictims = 0;
	agg_entries = palloc(hash_get_num_entries(pgss_agghash) * sizeof(pgssAggEntry *));
	hash_seq_init(&hash_dbseq, pgss_agghash);
	while ((agg_entry = hash_seq_search(&hash_dbseq)) != NULL)
	{
		if (agg_entry->key.bucket_id == bucket || bucket < 0)
			agg_entries[nvictims++] = agg_entry;
	}
	for (i = 0; i < nvictims; i++)
		hash_search(pgss_agghash, &agg_entries[i]->key, HASH_REMOVE, NULL);

	pfree(entries);
	pfree(agg_entries);
}

/*
 * Release all entries.
 */
static void
entry_reset()
{
	HASH_SEQ_STATUS		hash_seq;
	pgssEntry			*entry;
	pgssAggEntry		*dbentry;
	pgssObjectEntry		*objentry;
	pgssWaitEventEntry	*weentry;

	LWLockAcquire(pgss->lock, LW_EXCLUSIVE);

	hash_seq_init(&hash_seq, pgss_hash);
	while ((entry = hash_seq_search(&hash_seq)) != NULL)
	{
		hash_search(pgss_hash, &entry->key, HASH_REMOVE, NULL);
	}

	hash_seq_init(&hash_seq, pgss_agghash);
	while ((dbentry = hash_seq_search(&hash_seq)) != NULL)
	{
		hash_search(pgss_agghash, &dbentry->key, HASH_REMOVE, NULL);
	}

	hash_seq_init(&hash_seq, pgss_buckethash);
    while ((objentry = hash_seq_search(&hash_seq)) != NULL)
    {
		hash_search(pgss_buckethash, &dbentry->key, HASH_REMOVE, NULL);
    }

	hash_seq_init(&hash_seq, pgss_waiteventshash);
	while ((weentry = hash_seq_search(&hash_seq)) != NULL)
	{
		hash_search(pgss_waiteventshash, &dbentry->key, HASH_REMOVE, NULL);
    }
	pgss->current_wbucket = 0;
	free(pgssWaitEventEntries);
    free(pgssBucketEntries);
	LWLockRelease(pgss->lock);
}

/*
 * AppendJumble: Append a value that is substantive in a given query to
 * the current jumble.
 */
static void
AppendJumble(pgssJumbleState *jstate, const unsigned char *item, Size size)
{
	unsigned char *jumble = jstate->jumble;
	Size		jumble_len = jstate->jumble_len;

	/*
	 * Whenever the jumble buffer is full, we hash the current contents and
	 * reset the buffer to contain just that hash value, thus relying on the
	 * hash to summarize everything so far.
	 */
	while (size > 0)
	{
		Size		part_size;

		if (jumble_len >= JUMBLE_SIZE)
		{
			uint64		start_hash;

			start_hash = DatumGetUInt64(hash_any_extended(jumble,
														  JUMBLE_SIZE, 0));
			memcpy(jumble, &start_hash, sizeof(start_hash));
			jumble_len = sizeof(start_hash);
		}
		part_size = Min(size, JUMBLE_SIZE - jumble_len);
		memcpy(jumble + jumble_len, item, part_size);
		jumble_len += part_size;
		item += part_size;
		size -= part_size;
	}
	jstate->jumble_len = jumble_len;
}

/*
 * Wrappers around AppendJumble to encapsulate details of serialization
 * of individual local variable elements.
 */
#define APP_JUMB(item) \
	AppendJumble(jstate, (const unsigned char *) &(item), sizeof(item))
#define APP_JUMB_STRING(str) \
	AppendJumble(jstate, (const unsigned char *) (str), strlen(str) + 1)

/*
 * JumbleQuery: Selectively serialize the query tree, appending significant
 * data to the "query jumble" while ignoring nonsignificant data.
 *
 * Rule of thumb for what to include is that we should ignore anything not
 * semantically significant (such as alias names) as well as anything that can
 * be deduced from child nodes (else we'd just be double-hashing that piece
 * of information).
 */
static void
JumbleQuery(pgssJumbleState *jstate, Query *query)
{
	Assert(IsA(query, Query));
	Assert(query->utilityStmt == NULL);

	APP_JUMB(query->commandType);
	/* resultRelation is usually predictable from commandType */
	JumbleExpr(jstate, (Node *) query->cteList);
	JumbleRangeTable(jstate, query->rtable);
	JumbleExpr(jstate, (Node *) query->jointree);
	JumbleExpr(jstate, (Node *) query->targetList);
	JumbleExpr(jstate, (Node *) query->onConflict);
	JumbleExpr(jstate, (Node *) query->returningList);
	JumbleExpr(jstate, (Node *) query->groupClause);
	JumbleExpr(jstate, (Node *) query->groupingSets);
	JumbleExpr(jstate, query->havingQual);
	JumbleExpr(jstate, (Node *) query->windowClause);
	JumbleExpr(jstate, (Node *) query->distinctClause);
	JumbleExpr(jstate, (Node *) query->sortClause);
	JumbleExpr(jstate, query->limitOffset);
	JumbleExpr(jstate, query->limitCount);
	/* we ignore rowMarks */
	JumbleExpr(jstate, query->setOperations);
}

/*
 * Jumble a range table
 */
static void
JumbleRangeTable(pgssJumbleState *jstate, List *rtable)
{
	ListCell   *lc;

	foreach(lc, rtable)
	{
		RangeTblEntry *rte = lfirst_node(RangeTblEntry, lc);

		APP_JUMB(rte->rtekind);
		switch (rte->rtekind)
		{
			case RTE_RELATION:
				APP_JUMB(rte->relid);
				JumbleExpr(jstate, (Node *) rte->tablesample);
				break;
			case RTE_SUBQUERY:
				JumbleQuery(jstate, rte->subquery);
				break;
			case RTE_JOIN:
				APP_JUMB(rte->jointype);
				break;
			case RTE_FUNCTION:
				JumbleExpr(jstate, (Node *) rte->functions);
				break;
			case RTE_TABLEFUNC:
				JumbleExpr(jstate, (Node *) rte->tablefunc);
				break;
			case RTE_VALUES:
				JumbleExpr(jstate, (Node *) rte->values_lists);
				break;
			case RTE_CTE:

				/*
				 * Depending on the CTE name here isn't ideal, but it's the
				 * only info we have to identify the referenced WITH item.
				 */
				APP_JUMB_STRING(rte->ctename);
				APP_JUMB(rte->ctelevelsup);
				break;
			case RTE_NAMEDTUPLESTORE:
				APP_JUMB_STRING(rte->enrname);
				break;
			default:
				elog(ERROR, "unrecognized RTE kind: %d", (int) rte->rtekind);
				break;
		}
	}
}

/*
 * Jumble an expression tree
 *
 * In general this function should handle all the same node types that
 * expression_tree_walker() does, and therefore it's coded to be as parallel
 * to that function as possible.  However, since we are only invoked on
 * queries immediately post-parse-analysis, we need not handle node types
 * that only appear in planning.
 *
 * Note: the reason we don't simply use expression_tree_walker() is that the
 * point of that function is to support tree walkers that don't care about
 * most tree node types, but here we care about all types.  We should complain
 * about any unrecognized node type.
 */
static void
JumbleExpr(pgssJumbleState *jstate, Node *node)
{
	ListCell   *temp;

	if (node == NULL)
		return;

	/* Guard against stack overflow due to overly complex expressions */
	check_stack_depth();

	/*
	 * We always emit the node's NodeTag, then any additional fields that are
	 * considered significant, and then we recurse to any child nodes.
	 */
	APP_JUMB(node->type);

	switch (nodeTag(node))
	{
		case T_Var:
			{
				Var		   *var = (Var *) node;

				APP_JUMB(var->varno);
				APP_JUMB(var->varattno);
				APP_JUMB(var->varlevelsup);
			}
			break;
		case T_Const:
			{
				Const	   *c = (Const *) node;

				/* We jumble only the constant's type, not its value */
				APP_JUMB(c->consttype);
				/* Also, record its parse location for query normalization */
				RecordConstLocation(jstate, c->location);
			}
			break;
		case T_Param:
			{
				Param	   *p = (Param *) node;

				APP_JUMB(p->paramkind);
				APP_JUMB(p->paramid);
				APP_JUMB(p->paramtype);
				/* Also, track the highest external Param id */
				if (p->paramkind == PARAM_EXTERN &&
					p->paramid > jstate->highest_extern_param_id)
					jstate->highest_extern_param_id = p->paramid;
			}
			break;
		case T_Aggref:
			{
				Aggref	   *expr = (Aggref *) node;

				APP_JUMB(expr->aggfnoid);
				JumbleExpr(jstate, (Node *) expr->aggdirectargs);
				JumbleExpr(jstate, (Node *) expr->args);
				JumbleExpr(jstate, (Node *) expr->aggorder);
				JumbleExpr(jstate, (Node *) expr->aggdistinct);
				JumbleExpr(jstate, (Node *) expr->aggfilter);
			}
			break;
		case T_GroupingFunc:
			{
				GroupingFunc *grpnode = (GroupingFunc *) node;

				JumbleExpr(jstate, (Node *) grpnode->refs);
			}
			break;
		case T_WindowFunc:
			{
				WindowFunc *expr = (WindowFunc *) node;

				APP_JUMB(expr->winfnoid);
				APP_JUMB(expr->winref);
				JumbleExpr(jstate, (Node *) expr->args);
				JumbleExpr(jstate, (Node *) expr->aggfilter);
			}
			break;
#if PG_VERSION_NUM >= 120000
		case T_SubscriptingRef:
			{
				SubscriptingRef *sbsref = (SubscriptingRef *) node;

				JumbleExpr(jstate, (Node *) sbsref->refupperindexpr);
				JumbleExpr(jstate, (Node *) sbsref->reflowerindexpr);
				JumbleExpr(jstate, (Node *) sbsref->refexpr);
				JumbleExpr(jstate, (Node *) sbsref->refassgnexpr);
			}
			break;
#else
		case T_ArrayRef:
			{
				ArrayRef   *aref = (ArrayRef *) node;

				JumbleExpr(jstate, (Node *) aref->refupperindexpr);
				JumbleExpr(jstate, (Node *) aref->reflowerindexpr);
				JumbleExpr(jstate, (Node *) aref->refexpr);
				JumbleExpr(jstate, (Node *) aref->refassgnexpr);
			}
			break;
#endif
		case T_FuncExpr:
			{
				FuncExpr   *expr = (FuncExpr *) node;

				APP_JUMB(expr->funcid);
				JumbleExpr(jstate, (Node *) expr->args);
			}
			break;
		case T_NamedArgExpr:
			{
				NamedArgExpr *nae = (NamedArgExpr *) node;

				APP_JUMB(nae->argnumber);
				JumbleExpr(jstate, (Node *) nae->arg);
			}
			break;
		case T_OpExpr:
		case T_DistinctExpr:	/* struct-equivalent to OpExpr */
		case T_NullIfExpr:		/* struct-equivalent to OpExpr */
			{
				OpExpr	   *expr = (OpExpr *) node;

				APP_JUMB(expr->opno);
				JumbleExpr(jstate, (Node *) expr->args);
			}
			break;
		case T_ScalarArrayOpExpr:
			{
				ScalarArrayOpExpr *expr = (ScalarArrayOpExpr *) node;

				APP_JUMB(expr->opno);
				APP_JUMB(expr->useOr);
				JumbleExpr(jstate, (Node *) expr->args);
			}
			break;
		case T_BoolExpr:
			{
				BoolExpr   *expr = (BoolExpr *) node;

				APP_JUMB(expr->boolop);
				JumbleExpr(jstate, (Node *) expr->args);
			}
			break;
		case T_SubLink:
			{
				SubLink    *sublink = (SubLink *) node;

				APP_JUMB(sublink->subLinkType);
				APP_JUMB(sublink->subLinkId);
				JumbleExpr(jstate, (Node *) sublink->testexpr);
				JumbleQuery(jstate, castNode(Query, sublink->subselect));
			}
			break;
		case T_FieldSelect:
			{
				FieldSelect *fs = (FieldSelect *) node;

				APP_JUMB(fs->fieldnum);
				JumbleExpr(jstate, (Node *) fs->arg);
			}
			break;
		case T_FieldStore:
			{
				FieldStore *fstore = (FieldStore *) node;

				JumbleExpr(jstate, (Node *) fstore->arg);
				JumbleExpr(jstate, (Node *) fstore->newvals);
			}
			break;
		case T_RelabelType:
			{
				RelabelType *rt = (RelabelType *) node;

				APP_JUMB(rt->resulttype);
				JumbleExpr(jstate, (Node *) rt->arg);
			}
			break;
		case T_CoerceViaIO:
			{
				CoerceViaIO *cio = (CoerceViaIO *) node;

				APP_JUMB(cio->resulttype);
				JumbleExpr(jstate, (Node *) cio->arg);
			}
			break;
		case T_ArrayCoerceExpr:
			{
				ArrayCoerceExpr *acexpr = (ArrayCoerceExpr *) node;

				APP_JUMB(acexpr->resulttype);
				JumbleExpr(jstate, (Node *) acexpr->arg);
				JumbleExpr(jstate, (Node *) acexpr->elemexpr);
			}
			break;
		case T_ConvertRowtypeExpr:
			{
				ConvertRowtypeExpr *crexpr = (ConvertRowtypeExpr *) node;

				APP_JUMB(crexpr->resulttype);
				JumbleExpr(jstate, (Node *) crexpr->arg);
			}
			break;
		case T_CollateExpr:
			{
				CollateExpr *ce = (CollateExpr *) node;

				APP_JUMB(ce->collOid);
				JumbleExpr(jstate, (Node *) ce->arg);
			}
			break;
		case T_CaseExpr:
			{
				CaseExpr   *caseexpr = (CaseExpr *) node;

				JumbleExpr(jstate, (Node *) caseexpr->arg);
				foreach(temp, caseexpr->args)
				{
					CaseWhen   *when = lfirst_node(CaseWhen, temp);

					JumbleExpr(jstate, (Node *) when->expr);
					JumbleExpr(jstate, (Node *) when->result);
				}
				JumbleExpr(jstate, (Node *) caseexpr->defresult);
			}
			break;
		case T_CaseTestExpr:
			{
				CaseTestExpr *ct = (CaseTestExpr *) node;

				APP_JUMB(ct->typeId);
			}
			break;
		case T_ArrayExpr:
			JumbleExpr(jstate, (Node *) ((ArrayExpr *) node)->elements);
			break;
		case T_RowExpr:
			JumbleExpr(jstate, (Node *) ((RowExpr *) node)->args);
			break;
		case T_RowCompareExpr:
			{
				RowCompareExpr *rcexpr = (RowCompareExpr *) node;

				APP_JUMB(rcexpr->rctype);
				JumbleExpr(jstate, (Node *) rcexpr->largs);
				JumbleExpr(jstate, (Node *) rcexpr->rargs);
			}
			break;
		case T_CoalesceExpr:
			JumbleExpr(jstate, (Node *) ((CoalesceExpr *) node)->args);
			break;
		case T_MinMaxExpr:
			{
				MinMaxExpr *mmexpr = (MinMaxExpr *) node;

				APP_JUMB(mmexpr->op);
				JumbleExpr(jstate, (Node *) mmexpr->args);
			}
			break;
		case T_SQLValueFunction:
			{
				SQLValueFunction *svf = (SQLValueFunction *) node;

				APP_JUMB(svf->op);
				/* type is fully determined by op */
				APP_JUMB(svf->typmod);
			}
			break;
		case T_XmlExpr:
			{
				XmlExpr    *xexpr = (XmlExpr *) node;

				APP_JUMB(xexpr->op);
				JumbleExpr(jstate, (Node *) xexpr->named_args);
				JumbleExpr(jstate, (Node *) xexpr->args);
			}
			break;
		case T_NullTest:
			{
				NullTest   *nt = (NullTest *) node;

				APP_JUMB(nt->nulltesttype);
				JumbleExpr(jstate, (Node *) nt->arg);
			}
			break;
		case T_BooleanTest:
			{
				BooleanTest *bt = (BooleanTest *) node;

				APP_JUMB(bt->booltesttype);
				JumbleExpr(jstate, (Node *) bt->arg);
			}
			break;
		case T_CoerceToDomain:
			{
				CoerceToDomain *cd = (CoerceToDomain *) node;

				APP_JUMB(cd->resulttype);
				JumbleExpr(jstate, (Node *) cd->arg);
			}
			break;
		case T_CoerceToDomainValue:
			{
				CoerceToDomainValue *cdv = (CoerceToDomainValue *) node;

				APP_JUMB(cdv->typeId);
			}
			break;
		case T_SetToDefault:
			{
				SetToDefault *sd = (SetToDefault *) node;

				APP_JUMB(sd->typeId);
			}
			break;
		case T_CurrentOfExpr:
			{
				CurrentOfExpr *ce = (CurrentOfExpr *) node;

				APP_JUMB(ce->cvarno);
				if (ce->cursor_name)
					APP_JUMB_STRING(ce->cursor_name);
				APP_JUMB(ce->cursor_param);
			}
			break;
		case T_NextValueExpr:
			{
				NextValueExpr *nve = (NextValueExpr *) node;

				APP_JUMB(nve->seqid);
				APP_JUMB(nve->typeId);
			}
			break;
		case T_InferenceElem:
			{
				InferenceElem *ie = (InferenceElem *) node;

				APP_JUMB(ie->infercollid);
				APP_JUMB(ie->inferopclass);
				JumbleExpr(jstate, ie->expr);
			}
			break;
		case T_TargetEntry:
			{
				TargetEntry *tle = (TargetEntry *) node;

				APP_JUMB(tle->resno);
				APP_JUMB(tle->ressortgroupref);
				JumbleExpr(jstate, (Node *) tle->expr);
			}
			break;
		case T_RangeTblRef:
			{
				RangeTblRef *rtr = (RangeTblRef *) node;

				APP_JUMB(rtr->rtindex);
			}
			break;
		case T_JoinExpr:
			{
				JoinExpr   *join = (JoinExpr *) node;

				APP_JUMB(join->jointype);
				APP_JUMB(join->isNatural);
				APP_JUMB(join->rtindex);
				JumbleExpr(jstate, join->larg);
				JumbleExpr(jstate, join->rarg);
				JumbleExpr(jstate, join->quals);
			}
			break;
		case T_FromExpr:
			{
				FromExpr   *from = (FromExpr *) node;

				JumbleExpr(jstate, (Node *) from->fromlist);
				JumbleExpr(jstate, from->quals);
			}
			break;
		case T_OnConflictExpr:
			{
				OnConflictExpr *conf = (OnConflictExpr *) node;

				APP_JUMB(conf->action);
				JumbleExpr(jstate, (Node *) conf->arbiterElems);
				JumbleExpr(jstate, conf->arbiterWhere);
				JumbleExpr(jstate, (Node *) conf->onConflictSet);
				JumbleExpr(jstate, conf->onConflictWhere);
				APP_JUMB(conf->constraint);
				APP_JUMB(conf->exclRelIndex);
				JumbleExpr(jstate, (Node *) conf->exclRelTlist);
			}
			break;
		case T_List:
			foreach(temp, (List *) node)
			{
				JumbleExpr(jstate, (Node *) lfirst(temp));
			}
			break;
		case T_IntList:
			foreach(temp, (List *) node)
			{
				APP_JUMB(lfirst_int(temp));
			}
			break;
		case T_SortGroupClause:
			{
				SortGroupClause *sgc = (SortGroupClause *) node;

				APP_JUMB(sgc->tleSortGroupRef);
				APP_JUMB(sgc->eqop);
				APP_JUMB(sgc->sortop);
				APP_JUMB(sgc->nulls_first);
			}
			break;
		case T_GroupingSet:
			{
				GroupingSet *gsnode = (GroupingSet *) node;

				JumbleExpr(jstate, (Node *) gsnode->content);
			}
			break;
		case T_WindowClause:
			{
				WindowClause *wc = (WindowClause *) node;

				APP_JUMB(wc->winref);
				APP_JUMB(wc->frameOptions);
				JumbleExpr(jstate, (Node *) wc->partitionClause);
				JumbleExpr(jstate, (Node *) wc->orderClause);
				JumbleExpr(jstate, wc->startOffset);
				JumbleExpr(jstate, wc->endOffset);
			}
			break;
		case T_CommonTableExpr:
			{
				CommonTableExpr *cte = (CommonTableExpr *) node;

				/* we store the string name because RTE_CTE RTEs need it */
				APP_JUMB_STRING(cte->ctename);
				JumbleQuery(jstate, castNode(Query, cte->ctequery));
			}
			break;
		case T_SetOperationStmt:
			{
				SetOperationStmt *setop = (SetOperationStmt *) node;

				APP_JUMB(setop->op);
				APP_JUMB(setop->all);
				JumbleExpr(jstate, setop->larg);
				JumbleExpr(jstate, setop->rarg);
			}
			break;
		case T_RangeTblFunction:
			{
				RangeTblFunction *rtfunc = (RangeTblFunction *) node;

				JumbleExpr(jstate, rtfunc->funcexpr);
			}
			break;
		case T_TableFunc:
			{
				TableFunc  *tablefunc = (TableFunc *) node;

				JumbleExpr(jstate, tablefunc->docexpr);
				JumbleExpr(jstate, tablefunc->rowexpr);
				JumbleExpr(jstate, (Node *) tablefunc->colexprs);
			}
			break;
		case T_TableSampleClause:
			{
				TableSampleClause *tsc = (TableSampleClause *) node;

				APP_JUMB(tsc->tsmhandler);
				JumbleExpr(jstate, (Node *) tsc->args);
				JumbleExpr(jstate, (Node *) tsc->repeatable);
			}
			break;
		default:
			/* Only a warning, since we can stumble along anyway */
			elog(INFO, "unrecognized node type: %d",
				 (int) nodeTag(node));
			break;
	}
}

/*
 * Record location of constant within query string of query tree
 * that is currently being walked.
 */
static void
RecordConstLocation(pgssJumbleState *jstate, int location)
{
	/* -1 indicates unknown or undefined location */
	if (location >= 0)
	{
		/* enlarge array if needed */
		if (jstate->clocations_count >= jstate->clocations_buf_size)
		{
			jstate->clocations_buf_size *= 2;
			jstate->clocations = (pgssLocationLen *)
				repalloc(jstate->clocations,
						 jstate->clocations_buf_size *
						 sizeof(pgssLocationLen));
		}
		jstate->clocations[jstate->clocations_count].location = location;
		/* initialize lengths to -1 to simplify fill_in_constant_lengths */
		jstate->clocations[jstate->clocations_count].length = -1;
		jstate->clocations_count++;
	}
}

/*
 * Generate a normalized version of the query string that will be used to
 * represent all similar queries.
 *
 * Note that the normalized representation may well vary depending on
 * just which "equivalent" query is used to create the hashtable entry.
 * We assume this is OK.
 *
 * If query_loc > 0, then "query" has been advanced by that much compared to
 * the original string start, so we need to translate the provided locations
 * to compensate.  (This lets us avoid re-scanning statements before the one
 * of interest, so it's worth doing.)
 *
 * *query_len_p contains the input string length, and is updated with
 * the result string length on exit.  The resulting string might be longer
 * or shorter depending on what happens with replacement of constants.
 *
 * Returns a palloc'd string.
 */
static char *
generate_normalized_query(pgssJumbleState *jstate, const char *query,
						  int query_loc, int *query_len_p, int encoding)
{
	char	   *norm_query;
	int			query_len = *query_len_p;
	int			i,
				norm_query_buflen,	/* Space allowed for norm_query */
				len_to_wrt,		/* Length (in bytes) to write */
				quer_loc = 0,	/* Source query byte location */
				n_quer_loc = 0, /* Normalized query byte location */
				last_off = 0,	/* Offset from start for previous tok */
				last_tok_len = 0;	/* Length (in bytes) of that tok */

	/*
	 * Get constants' lengths (core system only gives us locations).  Note
	 * this also ensures the items are sorted by location.
	 */
	fill_in_constant_lengths(jstate, query, query_loc);

	/*
	 * Allow for $n symbols to be longer than the constants they replace.
	 * Constants must take at least one byte in text form, while a $n symbol
	 * certainly isn't more than 11 bytes, even if n reaches INT_MAX.  We
	 * could refine that limit based on the max value of n for the current
	 * query, but it hardly seems worth any extra effort to do so.
	 */
	norm_query_buflen = query_len + jstate->clocations_count * 10;

	/* Allocate result buffer */
	norm_query = palloc(norm_query_buflen + 1);

	for (i = 0; i < jstate->clocations_count; i++)
	{
		int			off,		/* Offset from start for cur tok */
					tok_len;	/* Length (in bytes) of that tok */

		off = jstate->clocations[i].location;
		/* Adjust recorded location if we're dealing with partial string */
		off -= query_loc;

		tok_len = jstate->clocations[i].length;

		if (tok_len < 0)
			continue;			/* ignore any duplicates */

		/* Copy next chunk (what precedes the next constant) */
		len_to_wrt = off - last_off;
		len_to_wrt -= last_tok_len;

		Assert(len_to_wrt >= 0);
		memcpy(norm_query + n_quer_loc, query + quer_loc, len_to_wrt);
		n_quer_loc += len_to_wrt;

		/* And insert a param symbol in place of the constant token */
		n_quer_loc += sprintf(norm_query + n_quer_loc, "$%d",
							  i + 1 + jstate->highest_extern_param_id);

		quer_loc = off + tok_len;
		last_off = off;
		last_tok_len = tok_len;
	}

	/*
	 * We've copied up until the last ignorable constant.  Copy over the
	 * remaining bytes of the original query string.
	 */
	len_to_wrt = query_len - quer_loc;

	Assert(len_to_wrt >= 0);
	memcpy(norm_query + n_quer_loc, query + quer_loc, len_to_wrt);
	n_quer_loc += len_to_wrt;

	Assert(n_quer_loc <= norm_query_buflen);
	norm_query[n_quer_loc] = '\0';

	*query_len_p = n_quer_loc;
	return norm_query;
}

/*
 * Given a valid SQL string and an array of constant-location records,
 * fill in the textual lengths of those constants.
 *
 * The constants may use any allowed constant syntax, such as float literals,
 * bit-strings, single-quoted strings and dollar-quoted strings.  This is
 * accomplished by using the public API for the core scanner.
 *
 * It is the caller's job to ensure that the string is a valid SQL statement
 * with constants at the indicated locations.  Since in practice the string
 * has already been parsed, and the locations that the caller provides will
 * have originated from within the authoritative parser, this should not be
 * a problem.
 *
 * Duplicate constant pointers are possible, and will have their lengths
 * marked as '-1', so that they are later ignored.  (Actually, we assume the
 * lengths were initialized as -1 to start with, and don't change them here.)
 *
 * If query_loc > 0, then "query" has been advanced by that much compared to
 * the original string start, so we need to translate the provided locations
 * to compensate.  (This lets us avoid re-scanning statements before the one
 * of interest, so it's worth doing.)
 *
 * N.B. There is an assumption that a '-' character at a Const location begins
 * a negative numeric constant.  This precludes there ever being another
 * reason for a constant to start with a '-'.
 */
static void
fill_in_constant_lengths(pgssJumbleState *jstate, const char *query,
						 int query_loc)
{
	pgssLocationLen		*locs;
	core_yyscan_t		yyscanner;
	core_yy_extra_type	yyextra;
	core_YYSTYPE		yylval;
	YYLTYPE				yylloc;
	int					last_loc = -1;
	int					i;

	/*
	 * Sort the records by location so that we can process them in order while
	 * scanning the query text.
	 */
	if (jstate->clocations_count > 1)
		qsort(jstate->clocations, jstate->clocations_count,
			  sizeof(pgssLocationLen), comp_location);
	locs = jstate->clocations;

	/* initialize the flex scanner --- should match raw_parser() */
	yyscanner = scanner_init(query,
							 &yyextra,
#if PG_VERSION_NUM >= 120000
							&ScanKeywords,
							ScanKeywordTokens);
#else
							ScanKeywords,
							NumScanKeywords);
#endif
	/* we don't want to re-emit any escape string warnings */
	yyextra.escape_string_warning = false;

	/* Search for each constant, in sequence */
	for (i = 0; i < jstate->clocations_count; i++)
	{
		int			loc = locs[i].location;
		int			tok;

		/* Adjust recorded location if we're dealing with partial string */
		loc -= query_loc;

		Assert(loc >= 0);

		if (loc <= last_loc)
			continue;			/* Duplicate constant, ignore */

		/* Lex tokens until we find the desired constant */
		for (;;)
		{
			tok = core_yylex(&yylval, &yylloc, yyscanner);

			/* We should not hit end-of-string, but if we do, behave sanely */
			if (tok == 0)
				break;			/* out of inner for-loop */

			/*
			 * We should find the token position exactly, but if we somehow
			 * run past it, work with that.
			 */
			if (yylloc >= loc)
			{
				if (query[loc] == '-')
				{
					/*
					 * It's a negative value - this is the one and only case
					 * where we replace more than a single token.
					 *
					 * Do not compensate for the core system's special-case
					 * adjustment of location to that of the leading '-'
					 * operator in the event of a negative constant.  It is
					 * also useful for our purposes to start from the minus
					 * symbol.  In this way, queries like "select * from foo
					 * where bar = 1" and "select * from foo where bar = -2"
					 * will have identical normalized query strings.
					 */
					tok = core_yylex(&yylval, &yylloc, yyscanner);
					if (tok == 0)
						break;	/* out of inner for-loop */
				}

				/*
				 * We now rely on the assumption that flex has placed a zero
				 * byte after the text of the current token in scanbuf.
				 */
				locs[i].length = strlen(yyextra.scanbuf + loc);
				break;			/* out of inner for-loop */
			}
		}

		/* If we hit end-of-string, give up, leaving remaining lengths -1 */
		if (tok == 0)
			break;

		last_loc = loc;
	}

	scanner_finish(yyscanner);
}

/*
 * comp_location: comparator for qsorting pgssLocationLen structs by location
 */
static int
comp_location(const void *a, const void *b)
{
	int	l = ((const pgssLocationLen *) a)->location;
	int	r = ((const pgssLocationLen *) b)->location;

	if (l < r)
		return -1;
	else if (l > r)
		return +1;
	else
		return 0;
}

/* Convert array into Text dataum */
static Datum
array_get_datum(int arr[])
{
	int     j;
	char    str[1024] = {0};
	char    tmp[10];
	bool    first = true;

	memset(str, 0, 1024);
	/* Need to calculate the actual size, and avoid unnessary memory usage */
	for (j = 0; j < 10; j++)
	{
		if (first)
		{
			snprintf(tmp, 10, "%d", arr[j]);
			strcat(str,tmp);
			first = false;
			continue;
		}
		snprintf(tmp, 10, ", %d", arr[j]);
		strcat(str,tmp);
	}
	return CStringGetTextDatum(str);
}

/* Alocate memory for a new entry */
void add_object_entry(uint64 queryid, char *objects)
{
	pgssObjectEntry	*entry = NULL;
	bool			found;
	pgssObjectHashKey key;

	key.queryid = queryid;
	entry = (pgssObjectEntry *) hash_search(pgss_object_hash, &key, HASH_ENTER, &found);
	if (!found)
	{
		SpinLockAcquire(&entry->mutex);
		snprintf(entry->tables_name, MAX_REL_LEN, "%s", objects);
		SpinLockRelease(&entry->mutex);
	}
}

/* Alocate memory for a new entry */
static pgssAggEntry *
agg_entry_alloc(pgssAggHashKey *key)
{
	pgssAggEntry	*entry = NULL;
	bool			found;

	entry = (pgssAggEntry *) hash_search(pgss_agghash, key, HASH_ENTER, &found);
	if (!found)
	{
		SpinLockAcquire(&entry->mutex);
		memset(&entry->counters, 0, sizeof(pgssAggCounters));
		entry->counters.total_calls = 0;
		SpinLockRelease(&entry->mutex);
	}
	return entry;
}

static void
update_agg_counters(uint64 bucket, uint64 queryid, uint64 id, AGG_KEY type)
{
	pgssAggHashKey	key;
	pgssAggEntry	*entry;

	key.id = id;
	key.type = (int64) type;
	key.queryid = queryid;
	key.bucket_id = bucket;

	entry = agg_entry_alloc(&key);
	if (!entry)
		return;

	SpinLockAcquire(&entry->mutex);

	entry->key.queryid = queryid;
	entry->key.id = id;
	entry->key.type = key.type;
	entry->key.bucket_id = bucket;

	entry->counters.total_calls++;
	SpinLockRelease(&entry->mutex);
}

Datum
pg_stat_agg(PG_FUNCTION_ARGS)
{
	ReturnSetInfo		*rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	TupleDesc			tupdesc;
	Tuplestorestate		*tupstore;
	MemoryContext		per_query_ctx;
	MemoryContext		oldcontext;
	HASH_SEQ_STATUS		hash_seq;
	pgssAggEntry		*entry;

	/* hash table must exist already */
	if (!pgss || !pgss_agghash)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("pg_stat_monitor: must be loaded via shared_preload_libraries")));

	/* check to see if caller supports us returning a tuplestore */
	if (rsinfo == NULL || !IsA(rsinfo, ReturnSetInfo))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("pg_stat_monitor: set-valued function called in context that cannot accept a set")));

	if (!(rsinfo->allowedModes & SFRM_Materialize))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("pg_stat_monitor: materialize mode required, but it is not " \
						"allowed in this context")));

	/* Switch into long-lived context to construct returned data structures */
	per_query_ctx = rsinfo->econtext->ecxt_per_query_memory;
	oldcontext = MemoryContextSwitchTo(per_query_ctx);

	/* Build a tuple descriptor for our result type */
	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "pg_stat_monitor: return type must be a row type");

	if (tupdesc->natts != 4)
		elog(ERROR, "pg_stat_monitor: incorrect number of output arguments, required %d", tupdesc->natts);

	tupstore = tuplestore_begin_heap(true, false, work_mem);
	rsinfo->returnMode = SFRM_Materialize;
	rsinfo->setResult = tupstore;
	rsinfo->setDesc = tupdesc;

	MemoryContextSwitchTo(oldcontext);

	/*
	 * Get shared lock, load or reload the query text file if we must, and
	 * iterate over the hashtable entries.
	 *
	 * With a large hash table, we might be holding the lock rather longer
	 * than one could wish.  However, this only blocks creation of new hash
	 * table entries, and the larger the hash table the less likely that is to
	 * be needed.  So we can hope this is okay.  Perhaps someday we'll decide
	 * we need to partition the hash table to limit the time spent holding any
	 * one lock.
	 */
	LWLockAcquire(pgss->lock, LW_SHARED);
	hash_seq_init(&hash_seq, pgss_agghash);
	while ((entry = hash_seq_search(&hash_seq)) != NULL)
	{
		Datum		values[6];
		bool		nulls[6];
		int			i = 0;
		char		queryid_txt[32];

		memset(values, 0, sizeof(values));
		memset(nulls, 0, sizeof(nulls));

		sprintf(queryid_txt, "%08lX", entry->key.queryid);
		values[i++] = CStringGetTextDatum(queryid_txt);
		values[i++] = Int64GetDatumFast(entry->key.id);
		values[i++] = Int64GetDatumFast(entry->key.type);
		values[i++] = Int64GetDatumFast(entry->counters.total_calls);
		tuplestore_putvalues(tupstore, tupdesc, values, nulls);
	}

	/* clean up and return the tuplestore */
	LWLockRelease(pgss->lock);
	tuplestore_donestoring(tupstore);
	return 0;
}

#define FIFO_HEAD(b) pgss->query_fifo[b].head
#define FIFO_TAIL(b) pgss->query_fifo[b].tail

static uint64
locate_query(uint64 bucket_id, uint64 queryid, char * query)
{
	uint64 id = 0;
	uint64 len = 0;
	uint64 offset = 0;
	uint64 tail = FIFO_TAIL(bucket_id);
	unsigned char *buf = pgss_qbuf[bucket_id];

	while (FIFO_HEAD(bucket_id) != tail)
	{
		offset = 0;
		memcpy(&id, &buf[tail + offset], sizeof (uint64)); /* query id */

		offset += sizeof (uint64);
		memcpy(&len, &buf[tail + offset], sizeof (uint64)); /* query len */

		if (len  == 0)
			return 0;

		offset += sizeof (uint64);
		if (query != NULL)
		{
			memcpy(query, &buf[tail + offset], len); /* Actual query */
			query[len] = 0;
		}
		offset += len;

		if (id == queryid)
			return id;
		tail = (tail + offset) % query_buf_size_bucket;
	}
	return 0;
}

static void
store_query(uint64 queryid, const char *query, uint64 query_len)
{
    int 	next;
	int 	offset = 0;

	if (query_len > PGSM_QUERY_MAX_LEN)
		query_len = PGSM_QUERY_MAX_LEN;

	/* Already have query in the shared buffer, there
	 * is no need to add that again.
	 */
	if (locate_query(pgss->current_wbucket, queryid, NULL) == queryid)
		return;

	next = FIFO_HEAD(pgss->current_wbucket) + query_len + sizeof (uint64) + sizeof (uint64);
    if (next >= query_buf_size_bucket)
        next = 0;

	/* Buffer is full */
    if (next == FIFO_HEAD(pgss->current_wbucket))
    {
		elog(INFO, "pg_stat_monitor: no space left in shared_buffer");
		return;
	}

	offset = 0;
	memcpy(&pgss_qbuf[pgss->current_wbucket][FIFO_HEAD(pgss->current_wbucket)], &queryid, sizeof (uint64)); /* query id */
	offset += sizeof (uint64);

	memcpy(&pgss_qbuf[pgss->current_wbucket][FIFO_HEAD(pgss->current_wbucket) + offset], &query_len, sizeof (uint64)); /* query len */
	offset += sizeof (uint64);

	memcpy(&pgss_qbuf[pgss->current_wbucket][FIFO_HEAD(pgss->current_wbucket) + offset], query, query_len); /* actual query */

    pgss->query_fifo[pgss->current_wbucket].head = next;
}

#if PG_VERSION_NUM >= 130000
static PlannedStmt * pgss_planner_hook(Query *parse, const char *query_string, int cursorOptions, ParamListInfo boundParams)
#else
static PlannedStmt *pgss_planner_hook(Query *parse, int opt, ParamListInfo param)
#endif
{
	PlannedStmt *result;
    if (MyProc)
    {
        int i = MyProc - ProcGlobal->allProcs;
        if (pgssWaitEventEntries[i]->key.queryid != parse->queryId)
		    pgssWaitEventEntries[i]->key.queryid = parse->queryId;
	}
#if PG_VERSION_NUM >= 130000
	if (PGSM_TRACK_PLANNING && query_string
		&& parse->queryId != UINT64CONST(0))
	{
		instr_time	start;
		instr_time	duration;
		BufferUsage bufusage_start,
					bufusage;
		WalUsage	walusage_start,
					walusage;

		/* We need to track buffer usage as the planner can access them. */
		bufusage_start = pgBufferUsage;

		/*
		 * Similarly the planner could write some WAL records in some cases
		 * (e.g. setting a hint bit with those being WAL-logged)
		 */
		walusage_start = pgWalUsage;
		INSTR_TIME_SET_CURRENT(start);

		plan_nested_level++;
		PG_TRY();
		{
    		if (planner_hook_next)
        		result = planner_hook_next(parse, query_string, cursorOptions, boundParams);
    		result = standard_planner(parse, query_string, cursorOptions, boundParams);
		}
		PG_FINALLY();
		{
			plan_nested_level--;
		}
		PG_END_TRY();

		INSTR_TIME_SET_CURRENT(duration);
		INSTR_TIME_SUBTRACT(duration, start);

		/* calc differences of buffer counters. */
		memset(&bufusage, 0, sizeof(BufferUsage));
		BufferUsageAccumDiff(&bufusage, &pgBufferUsage, &bufusage_start);

		/* calc differences of WAL counters. */
		memset(&walusage, 0, sizeof(WalUsage));
		WalUsageAccumDiff(&walusage, &pgWalUsage, &walusage_start);
		pgss_store(query_string,
				   parse->queryId,
				   parse->stmt_location,
				   parse->stmt_len,
				   PGSS_PLAN,
				   INSTR_TIME_GET_MILLISEC(duration),
				   0,
				   &bufusage,
				   &walusage,
				   NULL,
				   0,
				   0);
	}
	else
	{

    		if (planner_hook_next)
        		result = planner_hook_next(parse, query_string, cursorOptions, boundParams);
    		result = standard_planner(parse, query_string, cursorOptions, boundParams);
	}
#else
    if (planner_hook_next)
		result = planner_hook_next(parse, opt, param);
	result = standard_planner(parse, opt, param);
#endif
	return result;
}

static void
update_wait_event(void)
{
	PGPROC	*proc = NULL;
	int 	i;

	LWLockAcquire(ProcArrayLock, LW_SHARED);
	for (i = 0; i < ProcGlobal->allProcCount; i++)
    {
        proc = &ProcGlobal->allProcs[i];
		if (proc->pid == 0)
			continue;

		pgssWaitEventEntries[i]->wait_event_info = proc->wait_event_info;
		pgssWaitEventEntries[i]->pid = proc->pid;
	}
	LWLockRelease(ProcArrayLock);
}

static void
handle_sigterm(SIGNAL_ARGS)
{
    sigterm = true;
}

static void
register_wait_event(void)
{
    BackgroundWorker worker;

    memset(&worker, 0, sizeof(worker));
    worker.bgw_flags = BGWORKER_SHMEM_ACCESS;
    worker.bgw_start_time = BgWorkerStart_ConsistentState;
    worker.bgw_restart_time = 0;
    worker.bgw_notify_pid = 0;
    snprintf(worker.bgw_library_name, BGW_MAXLEN, "pg_stat_monitor");
    snprintf(worker.bgw_function_name, BGW_MAXLEN, CppAsString(wait_event_main));
    snprintf(worker.bgw_name, BGW_MAXLEN, "pg_stat_monitor collector");
    worker.bgw_main_arg = (Datum) 0;
    RegisterBackgroundWorker(&worker);
}

void
wait_event_main(Datum main_arg)
{
	int rc;

	InitPostgres(NULL, InvalidOid, NULL, InvalidOid, NULL, false);
	SetProcessingMode(NormalProcessing);
    pqsignal(SIGTERM, handle_sigterm);
    BackgroundWorkerUnblockSignals();
	while (1)
	{
        if (sigterm)
            break;
		rc = WaitLatch(&MyProc->procLatch, WL_LATCH_SET | WL_TIMEOUT | WL_POSTMASTER_DEATH, 1, PG_WAIT_EXTENSION);

		if (rc & WL_POSTMASTER_DEATH)
            proc_exit(1);

        ResetLatch(&MyProc->procLatch);

		update_wait_event();
	}
	proc_exit(0);
}

static uint64
get_query_id(pgssJumbleState *jstate, Query *query)
{
	uint64 queryid;

	/* Set up workspace for query jumbling */
	jstate->jumble = (unsigned char *) palloc(JUMBLE_SIZE);
	jstate->jumble_len = 0;
	jstate->clocations_buf_size = 32;
	jstate->clocations = (pgssLocationLen *) palloc(jstate->clocations_buf_size * sizeof(pgssLocationLen));
	jstate->clocations_count = 0;
	jstate->highest_extern_param_id = 0;

	/* Compute query ID and mark the Query node with it */
	JumbleQuery(jstate, query);
	queryid = DatumGetUInt64(hash_any_extended(jstate->jumble, jstate->jumble_len, 0));
	return queryid;
}

Datum
pg_stat_monitor_settings(PG_FUNCTION_ARGS)
{
	ReturnSetInfo		*rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	TupleDesc			tupdesc;
	Tuplestorestate		*tupstore;
	MemoryContext		per_query_ctx;
	MemoryContext		oldcontext;
	int					i;

	if (rsinfo == NULL || !IsA(rsinfo, ReturnSetInfo))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("pg_stat_monitor: set-valued function called in context that cannot accept a set")));

	/* Switch into long-lived context to construct returned data structures */
	per_query_ctx = rsinfo->econtext->ecxt_per_query_memory;
	oldcontext = MemoryContextSwitchTo(per_query_ctx);

	/* Build a tuple descriptor for our result type */
	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "pg_stat_monitor: return type must be a row type");

	if (tupdesc->natts != 7)
		elog(ERROR, "pg_stat_monitor: incorrect number of output arguments, required %d", tupdesc->natts);

	tupstore = tuplestore_begin_heap(true, false, work_mem);
	rsinfo->returnMode = SFRM_Materialize;
	rsinfo->setResult = tupstore;
	rsinfo->setDesc = tupdesc;

	MemoryContextSwitchTo(oldcontext);

	for(i = 0; i < 11; i++)
	{
		Datum		values[7];
		bool		nulls[7];
		int			j = 0;
		memset(values, 0, sizeof(values));
		memset(nulls, 0, sizeof(nulls));

		values[j++] = CStringGetTextDatum(conf[i].guc_name);
		values[j++] = Int64GetDatumFast(conf[i].guc_variable);
		values[j++] = Int64GetDatumFast(conf[i].guc_default);
		values[j++] = CStringGetTextDatum(conf[i].guc_desc);
		values[j++] = Int64GetDatumFast(conf[i].guc_min);
		values[j++] = Int64GetDatumFast(conf[i].guc_max);
		values[j++] = Int64GetDatumFast(conf[i].guc_restart);
		tuplestore_putvalues(tupstore, tupdesc, values, nulls);
	}
	/* clean up and return the tuplestore */
	tuplestore_donestoring(tupstore);
	return (Datum)0;
}
