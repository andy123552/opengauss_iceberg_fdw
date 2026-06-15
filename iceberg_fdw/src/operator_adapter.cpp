#include "postgres.h"

#include "optimizer/restrictinfo.h"

#include "iceberg_fdw.h"

static IcebergFdwSdkFilter *
icebergBuildEmptySdkFilter(void)
{
    IcebergFdwSdkFilter *filter = (IcebergFdwSdkFilter *)palloc0(sizeof(IcebergFdwSdkFilter));

    filter->predicates = NIL;
    filter->exact_in_sdk = false;
    return filter;
}

void
iceberg_operator_classify_scan_clauses(PlannerInfo *root, RelOptInfo *baserel, List *scan_clauses,
    List *column_mappings, IcebergFdwQualClassification *out)
{
    (void)root;
    (void)baserel;
    (void)column_mappings;

    if (out == NULL) {
        return;
    }

    out->sdk_predicates = NIL;
    out->local_exprs = extract_actual_clauses(scan_clauses, false);
    out->sdk_filter = icebergBuildEmptySdkFilter();
}
