#include "Python.h"
#include "postgres.h"
#include "access/relscan.h"
#include "catalog/pg_foreign_server.h"
#include "catalog/pg_foreign_table.h"
#include "catalog/pg_type.h"
#include "commands/defrem.h"
#include "commands/explain.h"
#include "foreign/fdwapi.h"
#include "foreign/foreign.h"
#include "funcapi.h"
#include "lib/stringinfo.h"
#include "nodes/bitmapset.h"
#include "nodes/makefuncs.h"
#include "nodes/pg_list.h"

#if PG_VERSION_NUM < 120000
#include "nodes/relation.h"
#endif
#include "utils/builtins.h"
#include "utils/syscache.h"

#ifndef PG_MULTICORN_H
#define PG_MULTICORN_H

/* Data structures */

#define C_LOG(...) do { \
	errstart(NOTICE, __FILE__, __LINE__, PG_FUNCNAME_MACRO, TEXTDOMAIN); \
	errmsg(__VA_ARGS__); \
	errfinish(0); \
} while (0)


typedef struct CacheEntry
{
	Oid			hashkey;
	PyObject   *value;
	List	   *options;
	List	   *columns;
	int			xact_depth;
	/* Keep the "options" and "columns" in a specific context to avoid leaks. */
	MemoryContext cacheContext;
}	CacheEntry;


typedef struct ConversionInfo
{
	char	   *attrname;
	FmgrInfo   *attinfunc;
	FmgrInfo   *attoutfunc;
	Oid			atttypoid;
	Oid			attioparam;
	int32		atttypmod;
	int			attnum;
	bool		is_array;
	int			attndims;
	bool		need_quote;
}	ConversionInfo;

/*
 * Context for multicorn_deparse_expr
 */
typedef struct deparse_expr_cxt
{
	PlannerInfo *root;			/* global planner state */
	RelOptInfo *foreignrel;		/* the foreign relation we are planning for */
	RelOptInfo *scanrel;		/* the underlying scan relation. Same as
								 * foreignrel, when that represents a join or
								 * a base relation. */

	StringInfo	buf;			/* output buffer to append to */
	List	  **params_list;	/* exprs that will become remote Params */
	bool		can_skip_cast;	/* outer function can skip int2/int4/int8/float4/float8 cast */
} deparse_expr_cxt;

/*
 * FDW-specific planner information kept in RelOptInfo.fdw_private for a
 * multicorn foreign table.
 * multicornGetForeignJoinPaths creates it for a joinrel (not implemented yet),
 * and mutlicornGetForeignUpperPaths creates it for an upperrel.
 */

typedef struct MulticornPlanState
{
	Oid			foreigntableid;
	AttrNumber	numattrs;
	PyObject   *fdw_instance;
	List	   *target_list;
	List	   *qual_list;
	int			startupCost;
	ConversionInfo **cinfos;
	List	   *pathkeys; /* list of MulticornDeparsedSortGroup) */

	/* For some reason, `baserel->reltarget->width` gets changed
	 * outside of our control somewhere between GetForeignPaths and
	 * GetForeignPlan, which breaks tests.
	 *
	 * XXX: This is very crude hack to transfer width, calculated by
	 * getRelSize to GetForeignPlan.
	 */
	int width;

	
    /* Details about upperrel pushdown fetched from the Python FDW instance */
    bool groupby_supported;
    List *agg_functions;

    /*
     * Aggregation and grouping data to be passed to the execution phase.
     * See MulticornExecState for more details.
     */
    List *upper_rel_targets;
	List *aggs;
	List *group_clauses;

    /*
	 * True means that the relation can be pushed down. Always true for simple
	 * foreign scan.
	 */
	bool		pushdown_safe;

	/* baserestrictinfo clauses, broken down into safe and unsafe subsets. */
	List	   *remote_conds;
	List	   *local_conds;

    /* Actual remote restriction clauses for scan (sans RestrictInfos) */
	List	   *final_remote_exprs;

	/* Estimated size and cost for a scan or join. */
	double		rows;
	Cost		startup_cost;
	Cost		total_cost;

	/* Costs excluding costs for transferring data from the foreign server */
	double		retrieved_rows;
	Cost		rel_startup_cost;
	Cost		rel_total_cost;

	/* Options extracted from catalogs. */
	bool		use_remote_estimate;
	Cost		fdw_startup_cost;
	Cost		fdw_tuple_cost;
	List	   *shippable_extensions;	/* OIDs of whitelisted extensions */

	/* Join information */
	RelOptInfo *outerrel;

	/* Upper relation information */
	UpperRelationKind stage;

	/* Cached catalog information. */
	ForeignTable *table;
	ForeignServer *server;
	UserMapping *user;			/* only set in use_remote_estimate mode */

	int			fetch_size;		/* fetch size for this remote table */

	/* Grouping information */
	List	   *grouped_tlist;

}	MulticornPlanState;

typedef struct MulticornExecState
{
	/* instance and iterator */
	PyObject   *fdw_instance;
	PyObject   *p_iterator;
	/* Information carried from the plan phase. */
	List	   *target_list;
	List	   *qual_list;
	Datum	   *values;
	bool	   *nulls;
	ConversionInfo **cinfos;
	    /*
     * List containing targets to be returned from Python in case of aggregations.
     * List elements are aggregation keys or group_clauses elements.
     */
    List *upper_rel_targets;
    /*
     * In case the query contains aggregations, the lists below details which
     * functions correspond to which columns.
     * List elements are themselves Lists of String nodes, denoting agg key,
     * operation and column names, respectively. The agg key corresponds to the
     * upper_rel_targets list entries.
     */
	List *aggs;
    /*
     * List containing GROUP BY information.
     * List elements are column names for grouping.
     */
	List *group_clauses;

	/* Common buffer to avoid repeated allocations */
	StringInfo	buffer;
	AttrNumber	rowidAttno;
	char	   *rowidAttrName;
	List	   *pathkeys; /* list of MulticornDeparsedSortGroup) */
	
    Relation	rel;			/* relcache entry for the foreign table. NULL
								 * for a foreign join scan. */
	TupleDesc	tupdesc;		/* tuple descriptor of scan */

}	MulticornExecState;

typedef struct MulticornModifyState
{
	ConversionInfo **cinfos;
	ConversionInfo **resultCinfos;
	PyObject   *fdw_instance;
	StringInfo	buffer;
	AttrNumber	rowidAttno;
	char	   *rowidAttrName;
	ConversionInfo *rowidCinfo;
}	MulticornModifyState;


typedef struct MulticornBaseQual
{
	AttrNumber	varattno;
	NodeTag		right_type;
	Oid			typeoid;
	char	   *opname;
	bool		isArray;
	bool		useOr;
}	MulticornBaseQual;

typedef struct MulticornConstQual
{
	MulticornBaseQual base;
	Datum		value;
	bool		isnull;
}	MulticornConstQual;

typedef struct MulticornVarQual
{
	MulticornBaseQual base;
	AttrNumber	rightvarattno;
}	MulticornVarQual;

typedef struct MulticornParamQual
{
	MulticornBaseQual base;
	Expr	   *expr;
}	MulticornParamQual;

typedef struct MulticornDeparsedSortGroup
{
	Name 			attname;
	int				attnum;
	bool			reversed;
	bool			nulls_first;
	Name			collate;
	PathKey	*key;
} MulticornDeparsedSortGroup;

/* deparse.c */
extern void multicorn_classify_conditions(PlannerInfo *root,
                                RelOptInfo *baserel,
                                List *input_conds,
                                List **remote_conds,
                                List **local_conds);
extern bool multicorn_is_foreign_expr(PlannerInfo *root,
								      RelOptInfo *baserel,
								      Expr *expr);
extern bool multicorn_is_foreign_param(PlannerInfo *root,
									   RelOptInfo *baserel,
									   Expr *expr);
extern List *multicorn_build_tlist_to_deparse(RelOptInfo *foreignrel);
extern void multicorn_extract_upper_rel_info(PlannerInfo *root, List *tlist, MulticornPlanState *fpinfo);


/* errors.c */
PGDLLEXPORT void		errorCheck(void);

/* python.c */
PGDLLEXPORT PyObject   *pgstringToPyUnicode(const char *string);
PGDLLEXPORT char	  **pyUnicodeToPgString(PyObject *pyobject);

PGDLLEXPORT PyObject   *getInstance(Oid foreigntableid);
PGDLLEXPORT PyObject   *qualToPyObject(Expr *expr, PlannerInfo *root);
PGDLLEXPORT PyObject   *getClassString(const char *className);
PGDLLEXPORT PyObject   *execute(ForeignScanState *state, ExplainState *es);
PGDLLEXPORT void pythonResultToTuple(PyObject *p_value,
					TupleTableSlot *slot,
					ConversionInfo ** cinfos,
					StringInfo buffer);
PGDLLEXPORT PyObject   *tupleTableSlotToPyObject(TupleTableSlot *slot, ConversionInfo ** cinfos);
PGDLLEXPORT char	   *getRowIdColumn(PyObject *fdw_instance);
PGDLLEXPORT PyObject   *optionsListToPyDict(List *options);
PGDLLEXPORT const char *getPythonEncodingName(void);
bool canPushdownUpperrel(MulticornPlanState * state);

PGDLLEXPORT void getRelSize(MulticornPlanState * state,
		PlannerInfo *root,
		double *rows,
		int *width);

PGDLLEXPORT List	   *pathKeys(MulticornPlanState * state);

PGDLLEXPORT List	   *canSort(MulticornPlanState * state, List *deparsed);

PGDLLEXPORT CacheEntry *getCacheEntry(Oid foreigntableid);
PGDLLEXPORT UserMapping *multicorn_GetUserMapping(Oid userid, Oid serverid);


/* Hash table mapping oid to fdw instances */
extern PGDLLEXPORT HTAB *InstancesHash;


/* query.c */
PGDLLEXPORT void extractRestrictions(Relids base_relids,
					Expr *node,
					List **quals);
PGDLLEXPORT List	   *extractColumns(List *reltargetlist, List *restrictinfolist);
PGDLLEXPORT void initConversioninfo(ConversionInfo ** cinfo,
		AttInMetadata *attinmeta, List *upper_rel_targets);

PGDLLEXPORT Value *colnameFromVar(Var *var, PlannerInfo *root);


PGDLLEXPORT void computeDeparsedSortGroup(List *deparsed, MulticornPlanState *planstate,
		List **apply_pathkeys,
		List **deparsed_pathkeys);

PGDLLEXPORT List	*findPaths(PlannerInfo *root, RelOptInfo *baserel, List *possiblePaths,
		int startupCost,
		MulticornPlanState *state,
		List *apply_pathkeys, List *deparsed_pathkeys);

PGDLLEXPORT List        *deparse_sortgroup(PlannerInfo *root, Oid foreigntableid, RelOptInfo *rel);

PGDLLEXPORT PyObject   *datumToPython(Datum node, Oid typeoid, ConversionInfo * cinfo);

PGDLLEXPORT List	*serializeDeparsedSortGroup(List *pathkeys);
PGDLLEXPORT List	*deserializeDeparsedSortGroup(List *items);

#endif   /* PG_MULTICORN_H */

PGDLLEXPORT char	   *PyUnicode_AsPgString(PyObject *p_unicode);

#if PY_MAJOR_VERSION >= 3
PGDLLEXPORT PyObject   *PyString_FromString(const char *s);
PGDLLEXPORT PyObject   *PyString_FromStringAndSize(const char *s, Py_ssize_t size);
PGDLLEXPORT char	   *PyString_AsString(PyObject *unicode);
PGDLLEXPORT int			PyString_AsStringAndSize(PyObject *unicode, char **tempbuffer, Py_ssize_t *length);

#endif
