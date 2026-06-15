CREATE EXTENSION IF NOT EXISTS iceberg_catalog;
CREATE EXTENSION IF NOT EXISTS iceberg_fdw;

DROP FOREIGN TABLE IF EXISTS managed_scan_table;
DROP SERVER IF EXISTS iceberg_scan_srv CASCADE;

CREATE SERVER iceberg_scan_srv
FOREIGN DATA WRAPPER iceberg_fdw
OPTIONS (
    warehouse 'file:///tmp/iceberg_fdw_regress'
);

CREATE FOREIGN TABLE managed_scan_table (
    c_smallint smallint,
    c_integer integer,
    c_bigint bigint,
    c_text text,
    c_varchar varchar(32),
    c_bpchar char(4),
    c_vector vector(3)
)
SERVER iceberg_scan_srv
OPTIONS (
    namespace 'scan_ns',
    table_name 'scan_table'
);

SELECT field_name, field_type
FROM iceberg_catalog.table_schemas
WHERE table_uuid = (
    SELECT table_uuid
    FROM iceberg_catalog.tables_internal
    WHERE relid = 'managed_scan_table'::regclass
)
ORDER BY field_position;

EXPLAIN (VERBOSE, COSTS OFF)
SELECT c_smallint, c_integer, c_bigint, c_text, c_varchar, c_bpchar::text, vector_dims(c_vector), c_vector::text
FROM managed_scan_table;

SELECT c_smallint, c_integer, c_bigint, c_text, c_varchar, c_bpchar::text AS c_bpchar_text,
       vector_dims(c_vector) AS c_vector_dims, c_vector::text AS c_vector_text
FROM managed_scan_table;

SELECT count(*) AS qual_match_rows
FROM managed_scan_table
WHERE c_integer = 42;

SELECT count(*) AS qual_miss_rows
FROM managed_scan_table
WHERE c_integer = 41;

DROP SERVER iceberg_scan_srv CASCADE;
