#include "postgres.h"

#include "access/tupdesc.h"
#include "executor/spi.h"
#include "lib/stringinfo.h"
#include "nodes/pg_list.h"
#include "utils/builtins.h"
#include "utils/elog.h"

#include "iceberg_fdw.h"

static char *
icebergQuoteLiteral(const char *value)
{
    return quote_literal_cstr(value == NULL ? "" : value);
}

static char *
icebergBuildTableUuid(Oid relid)
{
    return psprintf("00000000-0000-0000-0000-%012u", relid);
}

static void
icebergSpiExecuteOrError(const char *sql)
{
    int rc = SPI_execute(sql, false, 0);

    if (rc < 0) {
        ereport(ERROR,
            (errcode(ERRCODE_FDW_ERROR),
                errmsg("iceberg_fdw catalog SQL failed with SPI code %d", rc),
                errdetail_internal("%s", sql)));
    }
}

static void
icebergCatalogEnsureNamespace(const char *namespace_name)
{
    char *namespace_lit = icebergQuoteLiteral(namespace_name);
    char *sql = psprintf(
        "INSERT INTO iceberg_catalog.namespaces(catalog_name, namespace, properties) "
        "SELECT current_database()::text, %s, '{}'::jsonb "
        "WHERE NOT EXISTS ("
        "SELECT 1 FROM iceberg_catalog.namespaces "
        "WHERE catalog_name = current_database()::text AND namespace = %s)",
        namespace_lit,
        namespace_lit);

    icebergSpiExecuteOrError(sql);
}

static void
icebergCatalogInsertTable(const IcebergCatalogCreateTableRequest *request,
    const IcebergCatalogCreateTableResult *result)
{
    char *namespace_lit = icebergQuoteLiteral(request->namespace_name);
    char *table_lit = icebergQuoteLiteral(request->table_name);
    char *uuid_lit = icebergQuoteLiteral(result->table_uuid);
    char *metadata_lit = icebergQuoteLiteral(result->metadata_location);
    char *location_lit = icebergQuoteLiteral(request->table_location);
    char *sql = psprintf(
        "INSERT INTO iceberg_catalog.tables_internal("
        "relid, namespace, table_name, table_uuid, metadata_location, previous_metadata_location, "
        "table_location, last_column_id, current_schema_id, current_snapshot_id, default_spec_id) "
        "VALUES (%u::oid::regclass, %s, %s, %s::uuid, %s, NULL, %s, %d, %d, NULL, %d)",
        request->relid,
        namespace_lit,
        table_lit,
        uuid_lit,
        metadata_lit,
        location_lit,
        list_length(request->column_mappings),
        result->current_schema_id,
        ICEBERG_CATALOG_UNPARTITIONED_SPEC_ID);

    icebergSpiExecuteOrError(sql);
}

static void
icebergCatalogInsertSchemaFields(const IcebergCatalogCreateTableRequest *request,
    const IcebergCatalogCreateTableResult *result)
{
    ListCell *lc;
    int position = 0;

    foreach (lc, request->column_mappings) {
        IcebergFdwColumnMapping *mapping = (IcebergFdwColumnMapping *)lfirst(lc);
        char *uuid_lit = icebergQuoteLiteral(result->table_uuid);
        char *name_lit = icebergQuoteLiteral(mapping->field_name);
        char *type_lit = icebergQuoteLiteral(mapping->iceberg_type);
        char *sql = psprintf(
            "INSERT INTO iceberg_catalog.table_schemas("
            "table_uuid, schema_id, field_position, field_id, field_name, field_required, field_type, field_doc) "
            "VALUES (%s::uuid, %d, %d, %d, %s, %s, %s, NULL)",
            uuid_lit,
            result->current_schema_id,
            position++,
            mapping->field_id,
            name_lit,
            mapping->nullable ? "false" : "true",
            type_lit);

        icebergSpiExecuteOrError(sql);
    }
}

static void
icebergCatalogInsertUnpartitionedSpec(const IcebergCatalogCreateTableResult *result)
{
    char *uuid_lit = icebergQuoteLiteral(result->table_uuid);
    char *sql = psprintf(
        "INSERT INTO iceberg_catalog.partition_specs("
        "table_uuid, spec_id, field_position, field_id, source_id, field_name, transform) "
        "VALUES (%s::uuid, %d, -1, NULL, NULL, NULL, NULL)",
        uuid_lit,
        ICEBERG_CATALOG_UNPARTITIONED_SPEC_ID);

    icebergSpiExecuteOrError(sql);
}

bool
iceberg_catalog_create_managed_table(const IcebergCatalogCreateTableRequest *request,
    IcebergCatalogCreateTableResult *result)
{
    int rc;
    char *table_uuid;

    if (request->warehouse == NULL || request->warehouse[0] == '\0') {
        ereport(ERROR,
            (errcode(ERRCODE_FDW_OPTION_NAME_NOT_FOUND),
                errmsg("iceberg_fdw server option \"warehouse\" is required for managed table creation")));
    }

    table_uuid = icebergBuildTableUuid(request->relid);
    result->table_uuid = table_uuid;
    result->current_schema_id = 0;
    result->current_snapshot_id = 0;
    result->metadata_location = psprintf("%s/metadata/00000-%s.metadata.json", request->table_location, table_uuid);

    rc = SPI_connect();
    if (rc != SPI_OK_CONNECT) {
        ereport(ERROR,
            (errcode(ERRCODE_FDW_ERROR),
                errmsg("iceberg_fdw could not connect to SPI to write catalog metadata")));
    }

    PG_TRY();
    {
        icebergCatalogEnsureNamespace(request->namespace_name);
        icebergCatalogInsertTable(request, result);
        icebergCatalogInsertSchemaFields(request, result);
        icebergCatalogInsertUnpartitionedSpec(result);
        SPI_finish();
    }
    PG_CATCH();
    {
        SPI_finish();
        PG_RE_THROW();
    }
    PG_END_TRY();

    return true;
}

bool
iceberg_catalog_drop_managed_table(Oid relid)
{
    int rc;
    char *sql;

    if (!OidIsValid(relid)) {
        return false;
    }

    rc = SPI_connect();
    if (rc != SPI_OK_CONNECT) {
        ereport(ERROR,
            (errcode(ERRCODE_FDW_ERROR),
                errmsg("iceberg_fdw could not connect to SPI to drop catalog metadata")));
    }

    PG_TRY();
    {
        sql = psprintf(
            "DELETE FROM iceberg_catalog.tables_internal "
            "WHERE relid = %u::oid::regclass",
            relid);
        icebergSpiExecuteOrError(sql);
        SPI_finish();
    }
    PG_CATCH();
    {
        SPI_finish();
        PG_RE_THROW();
    }
    PG_END_TRY();

    return true;
}
