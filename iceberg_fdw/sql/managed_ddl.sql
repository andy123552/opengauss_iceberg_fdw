CREATE EXTENSION IF NOT EXISTS iceberg_catalog;
CREATE EXTENSION IF NOT EXISTS iceberg_fdw;

CREATE SERVER iceberg_managed_srv
FOREIGN DATA WRAPPER iceberg_fdw
OPTIONS (
    warehouse 'file:///tmp/iceberg_fdw_regress'
);

CREATE FOREIGN TABLE managed_orders (
    order_id bigint NOT NULL,
    user_id integer,
    small_status smallint,
    status varchar(32),
    note text,
    code char(4),
    embedding vector(3)
)
SERVER iceberg_managed_srv
OPTIONS (
    namespace 'regress_ns',
    table_name 'orders'
);

SELECT count(*) AS created_pg_class_rows
FROM pg_class
WHERE relname = 'managed_orders';

SELECT count(*) AS created_catalog_rows
FROM iceberg_catalog.tables_internal
WHERE relid = 'managed_orders'::regclass;

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

DO $$
BEGIN
    BEGIN
        CREATE FOREIGN TABLE managed_bad_vector (
            embedding vector
        )
        SERVER iceberg_managed_srv
        OPTIONS (
            namespace 'regress_ns',
            table_name 'bad_vector'
        );
    EXCEPTION WHEN OTHERS THEN
        RAISE NOTICE 'bad_vector rejected: %', SQLERRM;
    END;
END
$$;

SELECT count(*) AS bad_vector_catalog_rows
FROM iceberg_catalog.tables_internal
WHERE namespace = 'regress_ns' AND table_name = 'bad_vector';

DROP FOREIGN TABLE IF EXISTS managed_rollback;
DROP FOREIGN TABLE IF EXISTS managed_cascade_orders;

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

CREATE SERVER iceberg_cascade_srv
FOREIGN DATA WRAPPER iceberg_fdw
OPTIONS (
    warehouse 'file:///tmp/iceberg_fdw_regress'
);

CREATE FOREIGN TABLE managed_cascade_orders (
    id int
)
SERVER iceberg_cascade_srv
OPTIONS (
    namespace 'regress_ns',
    table_name 'cascade_orders'
);

CREATE TEMP TABLE managed_cascade_uuid AS
SELECT table_uuid
FROM iceberg_catalog.tables_internal
WHERE relid = 'managed_cascade_orders'::regclass;

DROP SERVER iceberg_cascade_srv CASCADE;

SELECT count(*) AS cascade_pg_class_rows
FROM pg_class
WHERE relname = 'managed_cascade_orders';

SELECT count(*) AS cascade_catalog_rows
FROM iceberg_catalog.tables_internal
WHERE namespace = 'regress_ns' AND table_name = 'cascade_orders';

SELECT count(*) AS cascade_schema_rows
FROM iceberg_catalog.table_schemas s
JOIN managed_cascade_uuid u ON s.table_uuid = u.table_uuid;

SELECT count(*) AS cascade_spec_rows
FROM iceberg_catalog.partition_specs p
JOIN managed_cascade_uuid u ON p.table_uuid = u.table_uuid;

CREATE TEMP TABLE managed_orders_uuid AS
SELECT table_uuid
FROM iceberg_catalog.tables_internal
WHERE relid = 'managed_orders'::regclass;

DROP FOREIGN TABLE managed_orders;

SELECT count(*) AS dropped_pg_class_rows
FROM pg_class
WHERE relname = 'managed_orders';

SELECT count(*) AS dropped_catalog_rows
FROM iceberg_catalog.tables_internal
WHERE namespace = 'regress_ns' AND table_name = 'orders';

SELECT count(*) AS dropped_schema_rows
FROM iceberg_catalog.table_schemas s
JOIN managed_orders_uuid u ON s.table_uuid = u.table_uuid;

SELECT count(*) AS dropped_spec_rows
FROM iceberg_catalog.partition_specs p
JOIN managed_orders_uuid u ON p.table_uuid = u.table_uuid;

DROP SERVER iceberg_managed_srv;
