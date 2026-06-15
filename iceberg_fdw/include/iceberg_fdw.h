#ifndef ICEBERG_FDW_H
#define ICEBERG_FDW_H

#include "postgres.h"
#include "access/tupdesc.h"
#include "nodes/parsenodes.h"
#include "nodes/pg_list.h"

#define ICEBERG_FDW_NAME "iceberg_fdw"
#define ICEBERG_CATALOG_SCHEMA "iceberg_catalog"
#define ICEBERG_CATALOG_UNPARTITIONED_SPEC_ID 0

typedef struct ArrowArray ArrowArray;
typedef struct ArrowSchema ArrowSchema;
typedef struct PlannerInfo PlannerInfo;
typedef struct RelOptInfo RelOptInfo;
typedef struct MemoryContextData *MemoryContext;
typedef struct TupleTableSlot TupleTableSlot;
typedef struct IcebergSdkScan IcebergSdkScan;
typedef struct DeltaScanHandle DeltaScanHandle;
typedef struct DeltaScanRequest DeltaScanRequest;

typedef enum IcebergFdwOptionContext {
    ICEBERG_FDW_OPTION_SERVER,
    ICEBERG_FDW_OPTION_TABLE,
    ICEBERG_FDW_OPTION_USER_MAPPING
} IcebergFdwOptionContext;

typedef struct IcebergFdwOptions {
    char *warehouse;
    char *namespace_name;
    char *table_name;
    char *snapshot_id;
    char *enable_index_scan;
} IcebergFdwOptions;

typedef struct IcebergFdwColumnMapping {
    AttrNumber attnum;
    int field_id;
    char *field_name;
    Oid pg_type;
    int32 pg_typmod;
    Oid pg_collation;
    bool nullable;
    char *iceberg_type;
} IcebergFdwColumnMapping;

typedef struct IcebergCatalogTableInfo {
    Oid relid;
    char *namespace_name;
    char *table_name;
    char *table_uuid;
    char *metadata_location;
    char *table_location;
    int current_schema_id;
    int64 current_snapshot_id;
} IcebergCatalogTableInfo;

typedef struct IcebergCatalogFieldInfo {
    int field_id;
    char *field_name;
    char *field_type;
    bool field_required;
    int field_position;
} IcebergCatalogFieldInfo;

typedef struct IcebergCatalogStats {
    bool has_total_records;
    double total_records;
    bool has_total_data_files;
    double total_data_files;
    bool has_total_file_size;
    double total_file_size;
} IcebergCatalogStats;

typedef struct IcebergFdwProjectedColumn {
    AttrNumber attnum;
    char *column_name;
    Oid pg_type;
    int32 pg_typmod;
    char *iceberg_field_type;
} IcebergFdwProjectedColumn;

typedef struct IcebergFdwScanEntry {
    Oid relid;
    char *table_uuid;
    char *metadata_location;
    int64 snapshot_id;
    int schema_id;
} IcebergFdwScanEntry;

typedef enum IcebergFdwOperator {
    ICEBERG_FDW_OP_EQ,
    ICEBERG_FDW_OP_LT,
    ICEBERG_FDW_OP_LE,
    ICEBERG_FDW_OP_GT,
    ICEBERG_FDW_OP_GE,
    ICEBERG_FDW_OP_IS_NULL,
    ICEBERG_FDW_OP_IS_NOT_NULL
} IcebergFdwOperator;

typedef struct IcebergFdwPredicate {
    char *column_name;
    Oid pg_type;
    int32 pg_typmod;
    char *iceberg_field_type;
    IcebergFdwOperator op;
    char *literal_value;
    bool exact_in_sdk;
} IcebergFdwPredicate;

typedef struct IcebergFdwSdkFilter {
    List *predicates;
    bool exact_in_sdk;
} IcebergFdwSdkFilter;

typedef struct IcebergFdwQualClassification {
    List *sdk_predicates;
    List *local_exprs;
    IcebergFdwSdkFilter *sdk_filter;
} IcebergFdwQualClassification;

typedef struct IcebergSdkScanRequest {
    const char *metadata_location;
    int64 snapshot_id;
    int schema_id;
    const char **columns;
    int n_columns;
    const IcebergFdwSdkFilter *filter;
    bool filter_is_exact;
    List *projected_columns;
} IcebergSdkScanRequest;

typedef struct IcebergCatalogCreateTableRequest {
    Oid relid;
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
extern void iceberg_ddl_ensure_hook(void);
extern void iceberg_ddl_validate_table_def(Node *obj);
extern List *iceberg_type_build_column_mappings(TupleDesc tuple_desc);
extern List *iceberg_type_build_column_mappings_from_relid(Oid relid);
extern List *iceberg_type_build_column_mappings_from_column_defs(List *column_defs);
extern Datum iceberg_type_literal_to_datum(const char *literal, Oid pg_type, int32 pg_typmod);
extern const char *iceberg_type_mock_literal_for_mapping(const IcebergFdwColumnMapping *mapping);
extern bool iceberg_catalog_get_table_info(Oid relid, IcebergCatalogTableInfo *out);
extern List *iceberg_catalog_get_fields(const char *table_uuid, int schema_id);
extern bool iceberg_catalog_get_snapshot_stats(const char *table_uuid, int64 snapshot_id,
    IcebergCatalogStats *out);
extern bool iceberg_catalog_create_managed_table(
    const IcebergCatalogCreateTableRequest *request,
    IcebergCatalogCreateTableResult *result);
extern bool iceberg_catalog_drop_managed_table(Oid relid);
extern void iceberg_metadata_track_create_table(
    Oid relid,
    const IcebergCatalogCreateTableResult *result);
extern bool iceberg_metadata_has_pending_changes(void);
extern void iceberg_metadata_commit_pending_changes(void);
extern void iceberg_metadata_abort_pending_changes(void);
extern void iceberg_operator_classify_scan_clauses(
    PlannerInfo *root,
    RelOptInfo *baserel,
    List *scan_clauses,
    List *column_mappings,
    IcebergFdwQualClassification *out);
extern IcebergSdkScan *iceberg_sdk_scan_open(
    MemoryContext cxt,
    const IcebergSdkScanRequest *request,
    ArrowSchema **out_schema);
extern int iceberg_sdk_scan_next(
    IcebergSdkScan *scan,
    ArrowArray **out_array,
    ArrowSchema **out_schema);
extern bool iceberg_sdk_scan_materialize_row(
    IcebergSdkScan *scan,
    TupleTableSlot *slot);
extern void iceberg_sdk_scan_release_batch(IcebergSdkScan *scan);
extern void iceberg_sdk_scan_close(IcebergSdkScan *scan);
extern Oid iceberg_delta_lookup_relation(Oid base_relid);
extern DeltaScanHandle *iceberg_delta_scan_begin(
    MemoryContext cxt,
    Oid delta_relid,
    const DeltaScanRequest *request);
extern bool iceberg_delta_scan_next(
    DeltaScanHandle *handle,
    TupleTableSlot *slot);
extern void iceberg_delta_scan_rescan(DeltaScanHandle *handle);
extern void iceberg_delta_scan_end(DeltaScanHandle *handle);

#endif
