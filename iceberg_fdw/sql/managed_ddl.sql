CREATE EXTENSION IF NOT EXISTS iceberg_catalog;
CREATE EXTENSION IF NOT EXISTS iceberg_fdw;

CREATE SERVER iceberg_managed_srv
FOREIGN DATA WRAPPER iceberg_fdw
OPTIONS (
    catalog_type 'internal',
    catalog_uri 'local',
    warehouse 'file:///tmp/iceberg_fdw_regress'
);

CREATE FOREIGN TABLE managed_orders (
    order_id bigint NOT NULL,
    user_id integer,
    small_status smallint,
    status varchar(32),
    note text,
    code char(4)
)
SERVER iceberg_managed_srv
OPTIONS (
    namespace 'regress_ns',
    table_name 'orders'
);

SELECT namespace, table_name, table_location, current_schema_id, current_snapshot_id IS NULL AS snapshot_is_null
FROM iceberg_catalog.tables_internal
WHERE relid = 'managed_orders'::regclass;

SELECT field_position, field_id, field_name, field_required, field_type
FROM iceberg_catalog.table_schemas
WHERE table_uuid = (
    SELECT table_uuid FROM iceberg_catalog.tables_internal WHERE relid = 'managed_orders'::regclass
)
ORDER BY field_position;

SELECT spec_id, field_position, field_id IS NULL AS field_id_is_null
FROM iceberg_catalog.partition_specs
WHERE table_uuid = (
    SELECT table_uuid FROM iceberg_catalog.tables_internal WHERE relid = 'managed_orders'::regclass
)
ORDER BY spec_id, field_position;

DO $$
BEGIN
    BEGIN
        CREATE FOREIGN TABLE managed_bad_path (
            id int
        )
        SERVER iceberg_managed_srv
        OPTIONS (
            namespace 'regress_ns',
            table_name 'bad_path',
            metadata_location 'file:///tmp/external.metadata.json'
        );
    EXCEPTION WHEN OTHERS THEN
        RAISE NOTICE 'bad_path rejected: %', SQLERRM;
    END;
END
$$;

SELECT count(*) AS bad_path_catalog_rows
FROM iceberg_catalog.tables_internal
WHERE namespace = 'regress_ns' AND table_name = 'bad_path';

DO $$
BEGIN
    BEGIN
        CREATE FOREIGN TABLE managed_missing_option (
            id int
        )
        SERVER iceberg_managed_srv
        OPTIONS (
            namespace 'regress_ns'
        );
    EXCEPTION WHEN OTHERS THEN
        RAISE NOTICE 'missing option rejected: %', SQLERRM;
    END;
END
$$;

DO $$
BEGIN
    BEGIN
        CREATE FOREIGN TABLE managed_bad_type (
            amount numeric(10, 2)
        )
        SERVER iceberg_managed_srv
        OPTIONS (
            namespace 'regress_ns',
            table_name 'bad_type'
        );
    EXCEPTION WHEN OTHERS THEN
        RAISE NOTICE 'bad_type rejected: %', SQLERRM;
    END;
END
$$;

SELECT count(*) AS bad_type_catalog_rows
FROM iceberg_catalog.tables_internal
WHERE namespace = 'regress_ns' AND table_name = 'bad_type';

BEGIN;
CREATE FOREIGN TABLE managed_rollback (
    id int
)
SERVER iceberg_managed_srv
OPTIONS (
    namespace 'regress_ns',
    table_name 'rollback_tbl'
);
ROLLBACK;

SELECT count(*) AS rollback_catalog_rows
FROM iceberg_catalog.tables_internal
WHERE namespace = 'regress_ns' AND table_name = 'rollback_tbl';

DROP FOREIGN TABLE managed_orders;
DROP SERVER iceberg_managed_srv;
