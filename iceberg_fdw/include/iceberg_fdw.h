#ifndef ICEBERG_FDW_H
#define ICEBERG_FDW_H

#include "postgres.h"
#include "nodes/pg_list.h"

typedef enum IcebergFdwOptionContext {
    ICEBERG_FDW_OPTION_SERVER,
    ICEBERG_FDW_OPTION_TABLE,
    ICEBERG_FDW_OPTION_USER_MAPPING
} IcebergFdwOptionContext;

typedef struct IcebergFdwOptions {
    char *catalog_type;
    char *catalog_uri;
    char *warehouse;
    char *namespace_name;
    char *table_name;
    char *snapshot_id;
    char *enable_index_scan;
} IcebergFdwOptions;

extern void icebergGetOptions(Oid foreigntableid, IcebergFdwOptions *options);

#endif
