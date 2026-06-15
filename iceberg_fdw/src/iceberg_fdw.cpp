#include "postgres.h"

#include "catalog/pg_foreign_server.h"
#include "catalog/pg_foreign_table.h"
#include "catalog/pg_user_mapping.h"
#include "commands/explain.h"
#include "executor/executor.h"
#include "foreign/fdwapi.h"
#include "foreign/foreign.h"
#include "nodes/makefuncs.h"
#include "optimizer/cost.h"
#include "optimizer/pathnode.h"
#include "optimizer/planmain.h"
#include "optimizer/restrictinfo.h"
#include "utils/builtins.h"
#include "utils/elog.h"
#include "utils/memutils.h"
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

typedef struct IcebergFdwScanState {
    Relation rel;
    Oid relid;
    TupleDesc tuple_desc;
    IcebergFdwScanEntry *scan_entry;
    List *projected_columns;
    IcebergFdwSdkFilter *sdk_filter;
    IcebergSdkScan *sdk_scan;
    DeltaScanHandle *delta_scan;
    bool delta_scan_enabled;
    bool reading_delta;
    MemoryContext scan_cxt;
    bool returned_base_row;
} IcebergFdwScanState;

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
static void icebergValidateTableDef(Node *obj);

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

    iceberg_ddl_ensure_hook();
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
    routine->ValidateTableDef = icebergValidateTableDef;

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

    iceberg_ddl_ensure_hook();

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
icebergValidateTableDef(Node *obj)
{
    if (obj == NULL) {
        return;
    }

    iceberg_ddl_validate_table_def(obj);
}

static List *
icebergBuildProjectedColumns(List *column_mappings)
{
    ListCell *lc;
    List *projected_columns = NIL;

    foreach (lc, column_mappings) {
        IcebergFdwColumnMapping *mapping = (IcebergFdwColumnMapping *)lfirst(lc);
        IcebergFdwProjectedColumn *projected = (IcebergFdwProjectedColumn *)palloc0(sizeof(IcebergFdwProjectedColumn));

        projected->attnum = mapping->attnum;
        projected->column_name = pstrdup(mapping->field_name);
        projected->pg_type = mapping->pg_type;
        projected->pg_typmod = mapping->pg_typmod;
        projected->iceberg_field_type = pstrdup(mapping->iceberg_type);
        projected_columns = lappend(projected_columns, projected);
    }

    return projected_columns;
}

static void
icebergGetForeignRelSize(PlannerInfo *root, RelOptInfo *baserel, Oid foreigntableid)
{
    (void)root;
    (void)foreigntableid;

    baserel->rows = 1;
    baserel->fdw_private = NULL;
}

static void
icebergGetForeignPaths(PlannerInfo *root, RelOptInfo *baserel, Oid foreigntableid)
{
    (void)foreigntableid;
    add_path(root, baserel, (Path *)create_foreignscan_path(root, baserel, 0, baserel->rows, NIL, NULL, NULL, NIL));
}

static ForeignScan *
icebergGetForeignPlan(PlannerInfo *root, RelOptInfo *baserel, Oid foreigntableid,
    ForeignPath *best_path, List *tlist, List *scan_clauses, Plan *outer_plan)
{
    IcebergFdwScanEntry *scan_entry;
    List *fdw_private;

    (void)root;
    (void)best_path;
    (void)foreigntableid;

    scan_clauses = extract_actual_clauses(scan_clauses, false);

    scan_entry = (IcebergFdwScanEntry *)palloc0(sizeof(IcebergFdwScanEntry));
    scan_entry->relid = baserel->relid;
    scan_entry->table_uuid = psprintf("mock-%u", baserel->relid);
    scan_entry->metadata_location = psprintf("mock://relation/%u", baserel->relid);
    scan_entry->snapshot_id = 0;
    scan_entry->schema_id = 0;
    fdw_private = list_make3(scan_entry, NIL, NIL);

    return make_foreignscan(tlist, scan_clauses, baserel->relid, NIL, fdw_private, NIL, NIL, outer_plan);
}

static void
icebergBeginForeignScan(ForeignScanState *node, int eflags)
{
    ForeignScan *fscan = (ForeignScan *)node->ss.ps.plan;
    IcebergFdwScanState *state;
    IcebergFdwScanEntry *scan_entry;
    List *projected_columns;
    IcebergFdwSdkFilter *sdk_filter;
    IcebergSdkScanRequest request;
    MemoryContext scan_cxt;
    Oid delta_relid;

    (void)eflags;

    if (fscan->fdw_private != NIL) {
        scan_entry = (IcebergFdwScanEntry *)linitial(fscan->fdw_private);
        projected_columns = (List *)lsecond(fscan->fdw_private);
        sdk_filter = (IcebergFdwSdkFilter *)lthird(fscan->fdw_private);
    } else {
        scan_entry = (IcebergFdwScanEntry *)palloc0(sizeof(IcebergFdwScanEntry));
        scan_entry->relid = RelationGetRelid(node->ss.ss_currentRelation);
        scan_entry->table_uuid = psprintf("mock-%u", scan_entry->relid);
        scan_entry->metadata_location = psprintf("mock://relation/%u", scan_entry->relid);
        scan_entry->snapshot_id = 0;
        scan_entry->schema_id = 0;
        projected_columns = NIL;
        sdk_filter = NULL;
    }

    scan_cxt = AllocSetContextCreate(node->ss.ps.state->es_query_cxt, "iceberg_fdw scan state",
        ALLOCSET_DEFAULT_SIZES);
    state = (IcebergFdwScanState *)MemoryContextAllocZero(scan_cxt, sizeof(IcebergFdwScanState));
    state->rel = node->ss.ss_currentRelation;
    state->relid = RelationGetRelid(node->ss.ss_currentRelation);
    state->tuple_desc = RelationGetDescr(node->ss.ss_currentRelation);
    state->scan_entry = scan_entry;
    state->projected_columns = projected_columns;
    state->sdk_filter = sdk_filter;
    state->scan_cxt = scan_cxt;
    state->returned_base_row = false;
    state->reading_delta = false;

    if (state->projected_columns == NIL) {
        List *column_mappings = iceberg_type_build_column_mappings(state->tuple_desc);

        state->projected_columns = icebergBuildProjectedColumns(column_mappings);
    }

    request.metadata_location = scan_entry->metadata_location;
    request.snapshot_id = scan_entry->snapshot_id;
    request.schema_id = scan_entry->schema_id;
    request.columns = NULL;
    request.n_columns = list_length(state->projected_columns);
    request.filter = sdk_filter;
    request.filter_is_exact = sdk_filter != NULL ? sdk_filter->exact_in_sdk : false;
    request.projected_columns = state->projected_columns;

    state->sdk_scan = iceberg_sdk_scan_open(scan_cxt, &request, NULL);

    delta_relid = iceberg_delta_lookup_relation(state->relid);
    if (OidIsValid(delta_relid)) {
        state->delta_scan = iceberg_delta_scan_begin(scan_cxt, delta_relid, NULL);
        state->delta_scan_enabled = state->delta_scan != NULL;
    }

    node->fdw_state = state;
}

static TupleTableSlot *
icebergIterateForeignScan(ForeignScanState *node)
{
    IcebergFdwScanState *state = (IcebergFdwScanState *)node->fdw_state;
    TupleTableSlot *slot = node->ss.ss_ScanTupleSlot;

    if (state == NULL) {
        return ExecClearTuple(slot);
    }

    for (;;) {
        if (!state->returned_base_row) {
            if (iceberg_sdk_scan_next(state->sdk_scan, NULL, NULL) > 0) {
                state->returned_base_row = true;
                if (iceberg_sdk_scan_materialize_row(state->sdk_scan, slot)) {
                    return slot;
                }
            } else {
                state->returned_base_row = true;
            }
        }

        if (!state->reading_delta) {
            state->reading_delta = true;
            continue;
        }

        if (state->delta_scan_enabled && iceberg_delta_scan_next(state->delta_scan, slot)) {
            return slot;
        }

        return ExecClearTuple(slot);
    }
}

static void
icebergReScanForeignScan(ForeignScanState *node)
{
    IcebergFdwScanState *state = (IcebergFdwScanState *)node->fdw_state;

    if (state == NULL) {
        return;
    }

    state->returned_base_row = false;
    state->reading_delta = false;

    if (state->delta_scan != NULL) {
        iceberg_delta_scan_rescan(state->delta_scan);
    }
}

static void
icebergEndForeignScan(ForeignScanState *node)
{
    IcebergFdwScanState *state = (IcebergFdwScanState *)node->fdw_state;

    if (state == NULL) {
        return;
    }

    if (state->sdk_scan != NULL) {
        iceberg_sdk_scan_close(state->sdk_scan);
        state->sdk_scan = NULL;
    }

    if (state->delta_scan != NULL) {
        iceberg_delta_scan_end(state->delta_scan);
        state->delta_scan = NULL;
    }

    if (state->scan_cxt != NULL) {
        MemoryContextDelete(state->scan_cxt);
    }

    node->fdw_state = NULL;
}

static void
icebergExplainForeignScan(ForeignScanState *node, ExplainState *es)
{
    ForeignScan *fscan = (ForeignScan *)node->ss.ps.plan;
    IcebergFdwScanEntry *scan_entry = NULL;
    List *projected_columns = NIL;
    IcebergFdwSdkFilter *sdk_filter = NULL;
    IcebergFdwScanState *state = (IcebergFdwScanState *)node->fdw_state;
    StringInfoData columns;
    ListCell *lc;

    if (state != NULL) {
        scan_entry = state->scan_entry;
        projected_columns = state->projected_columns;
        sdk_filter = state->sdk_filter;
    } else if (fscan != NULL && fscan->fdw_private != NIL) {
        scan_entry = (IcebergFdwScanEntry *)linitial(fscan->fdw_private);
        projected_columns = (List *)lsecond(fscan->fdw_private);
        sdk_filter = (IcebergFdwSdkFilter *)lthird(fscan->fdw_private);
    }

    initStringInfo(&columns);
    foreach (lc, projected_columns) {
        IcebergFdwProjectedColumn *column = (IcebergFdwProjectedColumn *)lfirst(lc);

        if (columns.len > 0) {
            appendStringInfoChar(&columns, ',');
        }
        appendStringInfoString(&columns, column->column_name);
    }

    ExplainPropertyText("Iceberg FDW", "mock full scan", es);
    if (scan_entry != NULL) {
        ExplainPropertyText("Iceberg Table UUID", scan_entry->table_uuid, es);
        ExplainPropertyText("Iceberg Metadata", scan_entry->metadata_location, es);
    }
    ExplainPropertyText("Projection Columns", columns.data, es);
    ExplainPropertyText("Delta Scan", (state != NULL && state->delta_scan_enabled) ? "enabled" : "disabled", es);
    ExplainPropertyText("SDK Filter", (sdk_filter != NULL && sdk_filter->predicates != NIL) ? "present" : "none", es);
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
