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

## Managed DDL Usage

The current implementation supports managed Iceberg foreign tables. The DDL
path writes table metadata into `iceberg_catalog` and expects the catalog
extension to be available in the database.

Create the required extensions:

```sql
CREATE EXTENSION IF NOT EXISTS iceberg_catalog;
CREATE EXTENSION IF NOT EXISTS iceberg_fdw;
```

Create a server with the managed-table catalog settings:

```sql
CREATE SERVER iceberg_managed_srv
  FOREIGN DATA WRAPPER iceberg_fdw
  OPTIONS (
    catalog_type 'internal',
    catalog_uri 'local',
    warehouse 'file:///tmp/iceberg_fdw_demo'
  );
```

Create a managed foreign table with `namespace` and `table_name`:

```sql
CREATE FOREIGN TABLE managed_orders (
  order_id bigint NOT NULL,
  user_id integer,
  status varchar(32),
  note text
)
SERVER iceberg_managed_srv
OPTIONS (
  namespace 'demo_ns',
  table_name 'orders'
);
```

The table creation path currently supports `int2`, `int4`, `int8`, `text`,
`varchar`, `bpchar`, and fixed-dimension `vector(n)` columns. It rejects
`metadata_location` and `path` options, because the implementation is focused
on managed tables rather than pre-existing external metadata.

Read path and DML callbacks are still placeholders until the team-provided
scan and delta write APIs are linked in.
