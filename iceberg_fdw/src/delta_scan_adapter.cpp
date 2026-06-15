#include "postgres.h"

#include "utils/memutils.h"

#include "iceberg_fdw.h"

struct DeltaScanHandle {
    Oid delta_relid;
    MemoryContext cxt;
};

Oid
iceberg_delta_lookup_relation(Oid base_relid)
{
    (void)base_relid;
    return InvalidOid;
}

DeltaScanHandle *
iceberg_delta_scan_begin(MemoryContext cxt, Oid delta_relid, const DeltaScanRequest *request)
{
    (void)cxt;
    (void)delta_relid;
    (void)request;
    return NULL;
}

bool
iceberg_delta_scan_next(DeltaScanHandle *handle, TupleTableSlot *slot)
{
    (void)handle;
    (void)slot;
    return false;
}

void
iceberg_delta_scan_rescan(DeltaScanHandle *handle)
{
    (void)handle;
}

void
iceberg_delta_scan_end(DeltaScanHandle *handle)
{
    (void)handle;
}
