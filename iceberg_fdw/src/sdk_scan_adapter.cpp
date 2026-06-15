#include "postgres.h"

#include "access/tupdesc.h"
#include "executor/tuptable.h"
#include "utils/datum.h"
#include "utils/memutils.h"

#include "iceberg_fdw.h"

struct IcebergSdkScan {
    MemoryContext cxt;
    IcebergSdkScanRequest request;
    Datum *values;
    bool *isnull;
    int nrows;
    int emitted_rows;
};

static void
icebergMockFillRow(IcebergSdkScan *scan)
{
    ListCell *lc;
    int i = 0;

    foreach (lc, scan->request.projected_columns) {
        IcebergFdwProjectedColumn *column = (IcebergFdwProjectedColumn *)lfirst(lc);
        IcebergFdwColumnMapping mapping;
        const char *literal;

        mapping.field_name = column->column_name;
        mapping.pg_type = column->pg_type;
        mapping.pg_typmod = column->pg_typmod;

        literal = iceberg_type_mock_literal_for_mapping(&mapping);
        scan->values[i] = iceberg_type_literal_to_datum(literal, column->pg_type, column->pg_typmod);
        scan->isnull[i] = false;
        i++;
    }

    scan->nrows = 1;
}

IcebergSdkScan *
iceberg_sdk_scan_open(MemoryContext cxt, const IcebergSdkScanRequest *request, ArrowSchema **out_schema)
{
    IcebergSdkScan *scan;
    MemoryContext scan_cxt;
    int n_columns = list_length(request->projected_columns);

    if (out_schema != NULL) {
        *out_schema = NULL;
    }

    scan_cxt = AllocSetContextCreate(cxt, "iceberg_fdw mock sdk scan", ALLOCSET_DEFAULT_SIZES);
    scan = (IcebergSdkScan *)MemoryContextAllocZero(scan_cxt, sizeof(IcebergSdkScan));
    scan->cxt = scan_cxt;
    scan->request = *request;
    scan->values = (Datum *)MemoryContextAllocZero(scan_cxt, sizeof(Datum) * n_columns);
    scan->isnull = (bool *)MemoryContextAllocZero(scan_cxt, sizeof(bool) * n_columns);
    icebergMockFillRow(scan);

    return scan;
}

int
iceberg_sdk_scan_next(IcebergSdkScan *scan, ArrowArray **out_array, ArrowSchema **out_schema)
{
    if (out_array != NULL) {
        *out_array = NULL;
    }
    if (out_schema != NULL) {
        *out_schema = NULL;
    }

    if (scan == NULL || scan->emitted_rows >= scan->nrows) {
        return 0;
    }

    scan->emitted_rows++;
    return 1;
}

bool
iceberg_sdk_scan_materialize_row(IcebergSdkScan *scan, TupleTableSlot *slot)
{
    ListCell *lc;
    int i = 0;

    if (scan == NULL || slot == NULL || slot->tts_tupleDescriptor == NULL) {
        return false;
    }

    for (int attno = 0; attno < slot->tts_tupleDescriptor->natts; attno++) {
        slot->tts_values[attno] = (Datum)0;
        slot->tts_isnull[attno] = true;
    }

    foreach (lc, scan->request.projected_columns) {
        IcebergFdwProjectedColumn *column = (IcebergFdwProjectedColumn *)lfirst(lc);
        Form_pg_attribute attr;
        int attno;

        attno = column->attnum - 1;
        if (attno < 0 || attno >= slot->tts_tupleDescriptor->natts) {
            i++;
            continue;
        }

        attr = TupleDescAttr(slot->tts_tupleDescriptor, attno);

        if (!scan->isnull[i]) {
            slot->tts_values[attno] = datumCopy(scan->values[i], attr->attbyval, attr->attlen);
        } else {
            slot->tts_values[attno] = (Datum)0;
        }
        slot->tts_isnull[attno] = scan->isnull[i];
        i++;
    }

    slot->tts_nvalid = slot->tts_tupleDescriptor->natts;
    ExecStoreVirtualTuple(slot);
    return true;
}

void
iceberg_sdk_scan_release_batch(IcebergSdkScan *scan)
{
    (void)scan;
}

void
iceberg_sdk_scan_close(IcebergSdkScan *scan)
{
    if (scan == NULL) {
        return;
    }

    if (scan->cxt != NULL) {
        MemoryContextDelete(scan->cxt);
    }
}
