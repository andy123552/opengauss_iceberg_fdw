# Project Agent: openGauss Iceberg FDW

## Role

This project tracks openGauss FDW integration for accessing Iceberg lake tables.
The agent should preserve project context, decompose implementation work, record
interface constraints, and follow existing openGauss FDW code patterns first.

## Project Summary

- Goal: access Iceberg tables from openGauss through FDW.
- Required capabilities include index scan and DML write support.
- Metadata management, low-level index structures, low-level index scan APIs,
  and delta-table write APIs are expected to be provided by the team.
- This project should implement the openGauss FDW-side planning, execution, DML,
  transaction, option validation, and type conversion flow.
- Current primary implementation direction is managed Iceberg foreign tables:
  Iceberg metadata is created and evolved through openGauss foreign-table DDL.
- Future scan evolution should consider choosing an index-backed scan during
  execution when the team index APIs are available. Vector top-k paths may be
  wrapped by an upper vector-search/refine step that performs local exact
  refinement in openGauss.

## Local Layout

- Project root: `/home/andy/opengauss_iceberg_fdw`
- openGauss source reference tree: `openGauss-server/`
- Source reference repository:
  `https://github.com/DataInfraLab/openGauss-server-datainfra`
- Source clone notes: `design/source-reference.md`
- Catalog source reference tree: `Catalog/`
- Catalog source reference repository: `https://github.com/HardingHang/Catalog`
- Catalog source reference commit:
  `8ed555bc4db70e7f1fd2ca5b3722e5dc159d1b57`
- FDW extension skeleton: `iceberg_fdw/`
- Runtime mode: use the Docker image in `docker-compose.yml`; do not compile
  the openGauss source tree by default.
- Main project overview: `README.md`
- Original context note: `AGENT.md`
- Docker service: `opengauss`
- Container name: `opengauss-iceberg-fdw`
- Host port: `15432`
- Default database password: `openGauss@123`

## Work Principles

- Do not reimplement Iceberg metadata, index, or delta write internals already
  provided by the team.
- Prefer calling existing team interfaces and clearly document interface
  boundaries.
- Before implementing, inspect openGauss FDW callbacks and contrib examples.
- Update `README.md` or add focused design docs whenever new constraints are
  discovered.
- Be conservative around DML, transaction handling, error recovery, rollback,
  and idempotency.
- Preserve unrelated local changes. The source tree may contain generated files
  from previous builds.

## Catalog Module Context

The team Catalog repository is cloned locally at `Catalog/` and is treated as
an external source reference, not as code to vendor into this repo. Keep it out
of normal commits.

Confirmed Catalog capabilities from commit
`8ed555bc4db70e7f1fd2ca5b3722e5dc159d1b57`:

- It is an openGauss extension named `iceberg_catalog`.
- The SQL extension creates schema `iceberg_catalog`.
- It defines metadata tables:
  `namespaces`, `tables_internal`, `table_schemas`, `snapshots`,
  `partition_specs`, and `tables_external`.
- It defines compatibility views:
  `iceberg_catalog.iceberg_tables` and
  `iceberg_catalog.iceberg_namespace_properties`.
- `tables_internal` is the FDW-relevant table binding local `relid` to
  `namespace`, `table_name`, `table_uuid`, `metadata_location`,
  `table_location`, `current_schema_id`, `current_snapshot_id`, and
  `default_spec_id`.
- `table_schemas` stores expanded top-level Iceberg schema fields by
  `table_uuid`, `schema_id`, `field_position`, and stable Iceberg `field_id`.
- `snapshots` stores snapshot summaries including `snapshot_id`, optional
  `schema_id`, `manifest_list`, and `total_records`.
- `partition_specs` stores partition-spec fields; an unpartitioned spec can be
  represented by `field_position = -1`.
- `tables_external` and the compatibility views are intended for JDBC Catalog
  style external records without local relation binding.

Current implementation boundary:

- `iceberg_catalog.create_table(...)` exists as a C-language SQL function, but
  its C implementation is currently a stub. It validates required arguments and
  returns a placeholder JSONB response.
- The C stub contains TODOs for schema validation, namespace/table existence
  checks, Iceberg SDK `CreateTable`, storage creation, and metadata-table
  registration.
- For FDW scan planning today, use the catalog metadata tables/views as the
  reliable integration surface. Do not assume the `create_table` function
  creates real Iceberg metadata until the TODOs are implemented.

## Current Development Principles

- The local `openGauss-server/` and `Catalog/` repositories are development
  references and integration dependencies. Keep `iceberg_fdw/` independently
  buildable while aligning headers, hooks, catalog tables, and runtime behavior
  with those repositories.
- Runtime debugging should use the Docker openGauss instance from this project.
  Build/install the development shared library into that instance for testing;
  the final product packaging and preload strategy are still open.
- Follow a test-first flow for feature work. Add target SQL/regression or unit
  tests before implementing behavior, then use them to guard each incremental
  step.
- Use `pg_lake/` as an external reference for DDL handling, extension test
  layout, and error-path coverage, but do not vendor pg_lake code into this
  project.
- Add focused unit/regression coverage whenever a development step introduces a
  new adapter boundary, catalog write, transaction callback, or type-mapping
  rule.

## Build Safety

- The previous local source build path caused OOM. Do not retry source
  compilation unless explicitly requested.
- Prefer Docker image installation and runtime checks.
- Before long openGauss or C/C++ builds, run `free -h`.
- Check for stale compiler/build processes with:
  `ps -eo pid,ppid,cmd | rg 'make|gcc|g\\+\\+|cc1|cc1plus'`
- Avoid unbounded parallelism. Use `MAKEFLAGS=-j2`, `make -j2`, or the nearest
  project-supported job flag.
- Do not use `make -j` or `-j$(nproc)` unless explicitly requested.
- Capture long build output to a log file and inspect with `tail` or `rg`.
- If memory pressure or swap usage rises sharply, stop and report the resource
  bottleneck.

## Likely Next Steps

1. Use `openGauss-server/` as a source reference for FDW callback and contrib
   implementation comparisons.
2. Confirm or repair local Docker-based openGauss runtime environment.
3. Record team-provided Iceberg metadata, index scan, and delta write APIs when
   available.
4. Implement a minimal Iceberg FDW extension skeleton.
5. Add managed foreign-table DDL hooks and full-scan execution first.
6. Add DML callbacks and transaction integration for delta writes.
7. Add index-backed scan and vector-search/refine integration after the basic
   managed full-scan path is stable.
