#ifndef ICEBERG_FDW_H
#define ICEBERG_FDW_H

#include "postgres.h"
#include "access/tupdesc.h"
#include "nodes/parsenodes.h"
#include "nodes/pg_list.h"

#define ICEBERG_FDW_NAME "iceberg_fdw"
#define ICEBERG_CATALOG_SCHEMA "iceberg_catalog"
#define ICEBERG_CATALOG_UNPARTITIONED_SPEC_ID 0

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

typedef enum IcebergFdwLogicalType {
    ICEBERG_FDW_TYPE_INT16,
    ICEBERG_FDW_TYPE_INT32,
    ICEBERG_FDW_TYPE_INT64,
    ICEBERG_FDW_TYPE_STRING,
    ICEBERG_FDW_TYPE_VECTOR_FLOAT32
} IcebergFdwLogicalType;

typedef struct IcebergFdwColumnMapping {
    AttrNumber attnum;
    int field_id;
    char *field_name;
    Oid pg_type;
    int32 pg_typmod;
    Oid pg_collation;
    IcebergFdwLogicalType logical_type;
    bool nullable;
    int vector_dim;
    char *iceberg_type;
} IcebergFdwColumnMapping;

typedef struct IcebergCatalogCreateTableRequest {
    Oid relid;
    const char *catalog_type;
    const char *catalog_uri;
    const char *warehouse;
    const char *namespace_name;
    const char *table_name;
    const char *table_location;
    TupleDesc tuple_desc;
    List *column_mappings;
} IcebergCatalogCreateTableRequest;

typedef struct IcebergCatalogCreateTableResult {
    char *table_uuid;
    char *metadata_location;
    int current_schema_id;
    int64 current_snapshot_id;
} IcebergCatalogCreateTableResult;

extern void icebergGetOptions(Oid foreigntableid, IcebergFdwOptions *options);
extern void iceberg_ddl_init(void);
extern void iceberg_ddl_fini(void);
extern List *iceberg_type_build_column_mappings(TupleDesc tuple_desc);
extern List *iceberg_type_build_column_mappings_from_column_defs(List *column_defs);
extern bool iceberg_catalog_create_managed_table(
    const IcebergCatalogCreateTableRequest *request,
    IcebergCatalogCreateTableResult *result);
extern void iceberg_metadata_track_create_table(
    Oid relid,
    const IcebergCatalogCreateTableResult *result);
extern bool iceberg_metadata_has_pending_changes(void);
extern void iceberg_metadata_commit_pending_changes(void);
extern void iceberg_metadata_abort_pending_changes(void);

#endif
