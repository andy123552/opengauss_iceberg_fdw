#include "postgres.h"

#include "nodes/pg_list.h"
#include "utils/elog.h"
#include "utils/memutils.h"

#include "iceberg_fdw.h"

typedef enum IcebergMetadataPendingOpKind {
    ICEBERG_METADATA_OP_CREATE_TABLE
} IcebergMetadataPendingOpKind;

typedef struct IcebergMetadataPendingOp {
    IcebergMetadataPendingOpKind kind;
    Oid relid;
    char *table_uuid;
    char *metadata_location;
} IcebergMetadataPendingOp;

static List *pending_metadata_ops = NIL;

void
iceberg_metadata_track_create_table(Oid relid, const IcebergCatalogCreateTableResult *result)
{
    MemoryContext txn_context = t_thrd.mem_cxt.cur_transaction_mem_cxt;
    MemoryContext old_context = MemoryContextSwitchTo(txn_context != NULL ? txn_context : CurrentMemoryContext);
    IcebergMetadataPendingOp *op = (IcebergMetadataPendingOp *)palloc0(sizeof(IcebergMetadataPendingOp));

    op->kind = ICEBERG_METADATA_OP_CREATE_TABLE;
    op->relid = relid;
    op->table_uuid = pstrdup(result->table_uuid);
    op->metadata_location = pstrdup(result->metadata_location);
    pending_metadata_ops = lappend(pending_metadata_ops, op);

    MemoryContextSwitchTo(old_context);
}

bool
iceberg_metadata_has_pending_changes(void)
{
    return pending_metadata_ops != NIL;
}

void
iceberg_metadata_commit_pending_changes(void)
{
    ListCell *lc;

    foreach (lc, pending_metadata_ops) {
        IcebergMetadataPendingOp *op = (IcebergMetadataPendingOp *)lfirst(lc);

        if (op->kind == ICEBERG_METADATA_OP_CREATE_TABLE) {
            ereport(DEBUG1,
                (errmsg("iceberg_fdw committed pending managed table metadata for relid %u", op->relid)));
        }
    }

    pending_metadata_ops = NIL;
}

void
iceberg_metadata_abort_pending_changes(void)
{
    pending_metadata_ops = NIL;
}
