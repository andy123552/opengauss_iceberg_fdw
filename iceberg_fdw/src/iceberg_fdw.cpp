#include "postgres.h"

#include "catalog/pg_foreign_server.h"
#include "catalog/pg_foreign_table.h"
#include "catalog/pg_user_mapping.h"
#include "commands/defrem.h"
#include "commands/explain.h"
#include "executor/executor.h"
#include "foreign/fdwapi.h"
#include "foreign/foreign.h"
#include "nodes/makefuncs.h"
#include "optimizer/pathnode.h"
#include "optimizer/planmain.h"
#include "optimizer/restrictinfo.h"
#include "utils/builtins.h"
#include "utils/elog.h"
#include "utils/rel.h"

#include "iceberg_fdw.h"

PG_MODULE_MAGIC;

extern List *untransformRelOptions(Datum options);

extern "C" Datum iceberg_fdw_handler(PG_FUNCTION_ARGS);
extern "C" Datum iceberg_fdw_validator(PG_FUNCTION_ARGS);
extern "C" void _PG_init(void);
extern "C" void _PG_fini(void);

PG_FUNCTION_INFO_V1(iceberg_fdw_handler);
PG_FUNCTION_INFO_V1(iceberg_fdw_validator);

typedef struct IcebergValidOption {
    const char *keyword;
    Oid optcontext;
} IcebergValidOption;

static const IcebergValidOption valid_options[] = {
    {"warehouse", ForeignServerRelationId},
    {"namespace", ForeignTableRelationId},
    {"table_name", ForeignTableRelationId},
    {"snapshot_id", ForeignTableRelationId},
    {"enable_index_scan", ForeignTableRelationId},
    {"username", UserMappingRelationId},
    {"password", UserMappingRelationId},
    {NULL, InvalidOid}
};

static void icebergGetForeignRelSize(PlannerInfo *root, RelOptInfo *baserel, Oid foreigntableid);
static void icebergGetForeignPaths(PlannerInfo *root, RelOptInfo *baserel, Oid foreigntableid);
static ForeignScan *icebergGetForeignPlan(PlannerInfo *root, RelOptInfo *baserel, Oid foreigntableid,
    ForeignPath *best_path, List *tlist, List *scan_clauses, Plan *outer_plan);
static void icebergBeginForeignScan(ForeignScanState *node, int eflags);
static TupleTableSlot *icebergIterateForeignScan(ForeignScanState *node);
static void icebergReScanForeignScan(ForeignScanState *node);
static void icebergEndForeignScan(ForeignScanState *node);
static void icebergExplainForeignScan(ForeignScanState *node, ExplainState *es);
static void icebergAddForeignUpdateTargets(Query *parsetree, RangeTblEntry *target_rte, Relation target_relation);
static List *icebergPlanForeignModify(PlannerInfo *root, ModifyTable *plan, Index resultRelation, int subplan_index);
static void icebergBeginForeignModify(ModifyTableState *mtstate, ResultRelInfo *resultRelInfo, List *fdw_private,
    int subplan_index, int eflags);
static TupleTableSlot *icebergExecForeignInsert(EState *estate, ResultRelInfo *resultRelInfo,
    TupleTableSlot *slot, TupleTableSlot *planSlot);
static TupleTableSlot *icebergExecForeignUpdate(EState *estate, ResultRelInfo *resultRelInfo,
    TupleTableSlot *slot, TupleTableSlot *planSlot);
static TupleTableSlot *icebergExecForeignDelete(EState *estate, ResultRelInfo *resultRelInfo,
    TupleTableSlot *slot, TupleTableSlot *planSlot);
static void icebergEndForeignModify(EState *estate, ResultRelInfo *resultRelInfo);

static bool
icebergIsValidOption(const char *keyword, Oid context)
{
    const IcebergValidOption *opt = valid_options;

    for (; opt->keyword != NULL; opt++) {
        if (context == opt->optcontext && strcmp(keyword, opt->keyword) == 0) {
            return true;
        }
    }

    return false;
}

extern "C" void
_PG_init(void)
{
    iceberg_ddl_init();
}

extern "C" void
_PG_fini(void)
{
    iceberg_ddl_fini();
}

static void
icebergApplyOption(IcebergFdwOptions *options, DefElem *def)
{
    if (strcmp(def->defname, "warehouse") == 0) {
        options->warehouse = defGetString(def);
    } else if (strcmp(def->defname, "namespace") == 0) {
        options->namespace_name = defGetString(def);
    } else if (strcmp(def->defname, "table_name") == 0) {
        options->table_name = defGetString(def);
    } else if (strcmp(def->defname, "snapshot_id") == 0) {
        options->snapshot_id = defGetString(def);
    } else if (strcmp(def->defname, "enable_index_scan") == 0) {
        options->enable_index_scan = defGetString(def);
    }
}

void
icebergGetOptions(Oid foreigntableid, IcebergFdwOptions *options)
{
    ForeignTable *table = GetForeignTable(foreigntableid);
    ForeignServer *server = GetForeignServer(table->serverid);
    ListCell *lc;

    memset(options, 0, sizeof(IcebergFdwOptions));

    foreach (lc, server->options) {
        icebergApplyOption(options, (DefElem *)lfirst(lc));
    }

    foreach (lc, table->options) {
        icebergApplyOption(options, (DefElem *)lfirst(lc));
    }
}

extern "C" Datum
iceberg_fdw_handler(PG_FUNCTION_ARGS)
{
    FdwRoutine *routine = makeNode(FdwRoutine);

    routine->GetForeignRelSize = icebergGetForeignRelSize;
    routine->GetForeignPaths = icebergGetForeignPaths;
    routine->GetForeignPlan = icebergGetForeignPlan;
    routine->BeginForeignScan = icebergBeginForeignScan;
    routine->IterateForeignScan = icebergIterateForeignScan;
    routine->ReScanForeignScan = icebergReScanForeignScan;
    routine->EndForeignScan = icebergEndForeignScan;
    routine->ExplainForeignScan = icebergExplainForeignScan;
    routine->AddForeignUpdateTargets = icebergAddForeignUpdateTargets;
    routine->PlanForeignModify = icebergPlanForeignModify;
    routine->BeginForeignModify = icebergBeginForeignModify;
    routine->ExecForeignInsert = icebergExecForeignInsert;
    routine->ExecForeignUpdate = icebergExecForeignUpdate;
    routine->ExecForeignDelete = icebergExecForeignDelete;
    routine->EndForeignModify = icebergEndForeignModify;

    PG_RETURN_POINTER(routine);
}

extern "C" Datum
iceberg_fdw_validator(PG_FUNCTION_ARGS)
{
    List *options_list = untransformRelOptions(PG_GETARG_DATUM(0));
    Oid catalog = PG_GETARG_OID(1);
    ListCell *cell;
    bool has_namespace = false;
    bool has_table_name = false;

    foreach (cell, options_list) {
        DefElem *def = (DefElem *)lfirst(cell);

        if (!icebergIsValidOption(def->defname, catalog)) {
            ereport(ERROR,
                (errcode(ERRCODE_FDW_INVALID_OPTION_NAME),
                    errmsg("invalid option \"%s\" for iceberg_fdw", def->defname)));
        }

        if (catalog == ForeignTableRelationId) {
            if (strcmp(def->defname, "namespace") == 0) {
                has_namespace = true;
            } else if (strcmp(def->defname, "table_name") == 0) {
                has_table_name = true;
            }
        }
    }

    if (catalog == ForeignTableRelationId) {
        if (!has_namespace) {
            ereport(ERROR,
                (errcode(ERRCODE_FDW_OPTION_NAME_NOT_FOUND),
                    errmsg("required option \"namespace\" is missing for iceberg_fdw managed foreign table")));
        }

        if (!has_table_name) {
            ereport(ERROR,
                (errcode(ERRCODE_FDW_OPTION_NAME_NOT_FOUND),
                    errmsg("required option \"table_name\" is missing for iceberg_fdw managed foreign table")));
        }
    }

    PG_RETURN_VOID();
}

static void
icebergGetForeignRelSize(PlannerInfo *root, RelOptInfo *baserel, Oid foreigntableid)
{
    IcebergFdwOptions options;

    icebergGetOptions(foreigntableid, &options);

    baserel->rows = 1000;
    baserel->fdw_private = NULL;
}

static void
icebergGetForeignPaths(PlannerInfo *root, RelOptInfo *baserel, Oid foreigntableid)
{
    Cost startup_cost = 0;
    Cost total_cost = baserel->rows;

    add_path(root, baserel, (Path *)create_foreignscan_path(root, baserel,
        startup_cost, total_cost, NIL, NULL, NULL, NIL));
}

static ForeignScan *
icebergGetForeignPlan(PlannerInfo *root, RelOptInfo *baserel, Oid foreigntableid,
    ForeignPath *best_path, List *tlist, List *scan_clauses, Plan *outer_plan)
{
    scan_clauses = extract_actual_clauses(scan_clauses, false);

    return make_foreignscan(tlist, scan_clauses, baserel->relid, NIL, NIL, NIL, NIL, outer_plan);
}

static void
icebergBeginForeignScan(ForeignScanState *node, int eflags)
{
    ereport(ERROR,
        (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
            errmsg("iceberg_fdw scan execution is not implemented yet")));
}

static TupleTableSlot *
icebergIterateForeignScan(ForeignScanState *node)
{
    return ExecClearTuple(node->ss.ss_ScanTupleSlot);
}

static void
icebergReScanForeignScan(ForeignScanState *node)
{
}

static void
icebergEndForeignScan(ForeignScanState *node)
{
}

static void
icebergExplainForeignScan(ForeignScanState *node, ExplainState *es)
{
    ExplainPropertyText("Iceberg FDW", "adapter callbacks not linked", es);
}

static void
icebergAddForeignUpdateTargets(Query *parsetree, RangeTblEntry *target_rte, Relation target_relation)
{
    ereport(ERROR,
        (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
            errmsg("iceberg_fdw UPDATE and DELETE row locator support is not implemented yet")));
}

static List *
icebergPlanForeignModify(PlannerInfo *root, ModifyTable *plan, Index resultRelation, int subplan_index)
{
    return NIL;
}

static void
icebergBeginForeignModify(ModifyTableState *mtstate, ResultRelInfo *resultRelInfo, List *fdw_private,
    int subplan_index, int eflags)
{
    ereport(ERROR,
        (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
            errmsg("iceberg_fdw DML execution is not implemented yet")));
}

static TupleTableSlot *
icebergExecForeignInsert(EState *estate, ResultRelInfo *resultRelInfo, TupleTableSlot *slot, TupleTableSlot *planSlot)
{
    return slot;
}

static TupleTableSlot *
icebergExecForeignUpdate(EState *estate, ResultRelInfo *resultRelInfo, TupleTableSlot *slot, TupleTableSlot *planSlot)
{
    return slot;
}

static TupleTableSlot *
icebergExecForeignDelete(EState *estate, ResultRelInfo *resultRelInfo, TupleTableSlot *slot, TupleTableSlot *planSlot)
{
    return slot;
}

static void
icebergEndForeignModify(EState *estate, ResultRelInfo *resultRelInfo)
{
}
