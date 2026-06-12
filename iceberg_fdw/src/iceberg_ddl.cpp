#include "postgres.h"

#include "access/xact.h"
#include "catalog/namespace.h"
#include "commands/defrem.h"
#include "foreign/foreign.h"
#include "nodes/parsenodes.h"
#include "tcop/utility.h"
#include "utils/elog.h"
#include "utils/rel.h"

#include "iceberg_fdw.h"

static ProcessUtility_hook_type prev_ProcessUtility_hook = NULL;

static void icebergProcessUtility(processutility_context *processutility_cxt,
    DestReceiver *dest,
#ifdef PGXC
    bool sentToRemote,
#endif
    char *completionTag,
    ProcessUtilityContext context,
    bool isCtas);
static void icebergXactCallback(XactEvent event, void *arg);

static bool
icebergIsTargetCreateForeignTable(Node *parse_tree)
{
    ForeignServer *server;
    ForeignDataWrapper *fdw;
    CreateForeignTableStmt *stmt;

    if (parse_tree == NULL || !IsA(parse_tree, CreateForeignTableStmt)) {
        return false;
    }

    stmt = (CreateForeignTableStmt *)parse_tree;
    server = GetForeignServerByName(stmt->servername, true);
    if (server == NULL) {
        return false;
    }

    fdw = GetForeignDataWrapper(server->fdwid);
    return strcmp(fdw->fdwname, ICEBERG_FDW_NAME) == 0;
}

static void
icebergCallParentProcessUtility(processutility_context *processutility_cxt,
    DestReceiver *dest,
#ifdef PGXC
    bool sentToRemote,
#endif
    char *completionTag,
    ProcessUtilityContext context,
    bool isCtas)
{
    if (prev_ProcessUtility_hook != NULL) {
        prev_ProcessUtility_hook(processutility_cxt, dest,
#ifdef PGXC
            sentToRemote,
#endif
            completionTag, context, isCtas);
    } else {
        standard_ProcessUtility(processutility_cxt, dest,
#ifdef PGXC
            sentToRemote,
#endif
            completionTag, context, isCtas);
    }
}

static const char *
icebergGetRequiredOption(List *options, const char *name)
{
    ListCell *lc;

    foreach (lc, options) {
        DefElem *def = (DefElem *)lfirst(lc);

        if (strcmp(def->defname, name) == 0) {
            const char *value = defGetString(def);

            if (value == NULL || value[0] == '\0') {
                ereport(ERROR,
                    (errcode(ERRCODE_FDW_INVALID_OPTION_NAME),
                        errmsg("iceberg_fdw option \"%s\" must not be empty", name)));
            }

            return value;
        }
    }

    ereport(ERROR,
        (errcode(ERRCODE_FDW_OPTION_NAME_NOT_FOUND),
            errmsg("iceberg_fdw option \"%s\" is required for managed Iceberg tables", name)));

    return NULL;
}

static void
icebergApplyDdlOption(IcebergFdwOptions *options, DefElem *def)
{
    if (strcmp(def->defname, "catalog_type") == 0) {
        options->catalog_type = defGetString(def);
    } else if (strcmp(def->defname, "catalog_uri") == 0) {
        options->catalog_uri = defGetString(def);
    } else if (strcmp(def->defname, "warehouse") == 0) {
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

static void
icebergGetDdlOptions(ForeignServer *server, CreateForeignTableStmt *stmt, IcebergFdwOptions *options)
{
    ListCell *lc;

    memset(options, 0, sizeof(IcebergFdwOptions));

    foreach (lc, server->options) {
        icebergApplyDdlOption(options, (DefElem *)lfirst(lc));
    }

    foreach (lc, stmt->options) {
        icebergApplyDdlOption(options, (DefElem *)lfirst(lc));
    }
}

static void
icebergRejectExternalPathOptions(List *options)
{
    ListCell *lc;

    foreach (lc, options) {
        DefElem *def = (DefElem *)lfirst(lc);

        if (strcmp(def->defname, "metadata_location") == 0 || strcmp(def->defname, "path") == 0) {
            ereport(ERROR,
                (errcode(ERRCODE_FDW_INVALID_OPTION_NAME),
                    errmsg("iceberg_fdw option \"%s\" is not supported for managed Iceberg tables", def->defname),
                    errdetail("Create managed tables with namespace and table_name instead of external metadata paths.")));
        }
    }
}

static char *
icebergBuildTableLocation(const char *warehouse, const char *namespace_name, const char *table_name)
{
    if (warehouse == NULL || warehouse[0] == '\0') {
        ereport(ERROR,
            (errcode(ERRCODE_FDW_OPTION_NAME_NOT_FOUND),
                errmsg("iceberg_fdw server option \"warehouse\" is required for managed Iceberg tables")));
    }

    if (warehouse[strlen(warehouse) - 1] == '/') {
        return psprintf("%s%s/%s", warehouse, namespace_name, table_name);
    }

    return psprintf("%s/%s/%s", warehouse, namespace_name, table_name);
}

static void
icebergHandleCreateForeignTable(CreateForeignTableStmt *stmt)
{
    ForeignServer *server = GetForeignServerByName(stmt->servername, false);
    Oid relid = RangeVarGetRelid(stmt->base.relation, AccessShareLock, false);
    IcebergFdwOptions options;
    List *column_mappings;
    IcebergCatalogCreateTableRequest request;
    IcebergCatalogCreateTableResult result;

    icebergGetDdlOptions(server, stmt, &options);
    icebergRejectExternalPathOptions(stmt->options);
    icebergGetRequiredOption(stmt->options, "namespace");
    icebergGetRequiredOption(stmt->options, "table_name");

    column_mappings = iceberg_type_build_column_mappings_from_column_defs(stmt->base.tableElts);

    memset(&request, 0, sizeof(request));
    memset(&result, 0, sizeof(result));
    request.relid = relid;
    request.catalog_type = options.catalog_type;
    request.catalog_uri = options.catalog_uri;
    request.warehouse = options.warehouse;
    request.namespace_name = options.namespace_name;
    request.table_name = options.table_name;
    request.table_location = icebergBuildTableLocation(options.warehouse, options.namespace_name, options.table_name);
    request.tuple_desc = NULL;
    request.column_mappings = column_mappings;

    iceberg_catalog_create_managed_table(&request, &result);
    iceberg_metadata_track_create_table(relid, &result);
}

static void
icebergProcessUtility(processutility_context *processutility_cxt,
    DestReceiver *dest,
#ifdef PGXC
    bool sentToRemote,
#endif
    char *completionTag,
    ProcessUtilityContext context,
    bool isCtas)
{
    Node *parse_tree = processutility_cxt->parse_tree;
    bool is_iceberg_create = icebergIsTargetCreateForeignTable(parse_tree);

    if (is_iceberg_create) {
        CreateForeignTableStmt *stmt = (CreateForeignTableStmt *)parse_tree;

        icebergRejectExternalPathOptions(stmt->options);
        icebergGetRequiredOption(stmt->options, "namespace");
        icebergGetRequiredOption(stmt->options, "table_name");
    }

    icebergCallParentProcessUtility(processutility_cxt, dest,
#ifdef PGXC
        sentToRemote,
#endif
        completionTag, context, isCtas);

    if (is_iceberg_create) {
        CommandCounterIncrement();
        icebergHandleCreateForeignTable((CreateForeignTableStmt *)parse_tree);
    }
}

static void
icebergXactCallback(XactEvent event, void *arg)
{
    switch (event) {
        case XACT_EVENT_PRE_COMMIT:
            iceberg_metadata_commit_pending_changes();
            break;
        case XACT_EVENT_ABORT:
            iceberg_metadata_abort_pending_changes();
            break;
        case XACT_EVENT_PRE_PREPARE:
            if (iceberg_metadata_has_pending_changes()) {
                ereport(ERROR,
                    (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                        errmsg("prepared transactions with iceberg_fdw metadata changes are not supported")));
            }
            break;
        default:
            break;
    }
}

void
iceberg_ddl_init(void)
{
    prev_ProcessUtility_hook = ProcessUtility_hook;
    ProcessUtility_hook = icebergProcessUtility;
    RegisterXactCallback(icebergXactCallback, NULL);
}

void
iceberg_ddl_fini(void)
{
    ProcessUtility_hook = prev_ProcessUtility_hook;
    UnregisterXactCallback(icebergXactCallback, NULL);
}
