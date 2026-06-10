# iceberg_fdw

`iceberg_fdw` is the openGauss FDW extension skeleton for accessing Iceberg
tables through team-provided metadata, index scan, and delta write interfaces.

This directory is intentionally limited to the openGauss FDW side:

- FDW handler and validator entry points.
- Foreign server, user mapping, and foreign table option validation.
- Planner, executor, DML, explain, and transaction lifecycle placeholders.
- Adapter boundaries for catalog, index, delta writer, and type conversion code.

It does not implement Iceberg metadata parsing, low-level index structures,
low-level scan engines, or delta table internals.

## Build

Use an openGauss development environment that provides `pg_config` and server
headers:

```bash
make PG_CONFIG=/path/to/pg_config
make install PG_CONFIG=/path/to/pg_config
```

The project root defaults to Docker runtime checks. Do not compile the full
`openGauss-server/` source tree unless that is explicitly required.

## SQL Usage

```sql
CREATE EXTENSION iceberg_fdw;

CREATE SERVER iceberg_srv
  FOREIGN DATA WRAPPER iceberg_fdw
  OPTIONS (
    catalog_type 'rest',
    catalog_uri 'http://catalog:8181',
    warehouse 's3://warehouse'
  );

CREATE FOREIGN TABLE iceberg_table (
  id bigint,
  data text
)
SERVER iceberg_srv
OPTIONS (
  namespace 'default',
  table_name 'sample_table'
);
```

The scan and DML callbacks currently raise feature-not-supported errors until
the team-provided adapter APIs are linked in.
