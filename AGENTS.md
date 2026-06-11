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
