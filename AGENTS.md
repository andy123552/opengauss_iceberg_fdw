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

## Local Layout

- Project root: `/home/andy/opengauss_iceberg_fdw`
- Runtime mode: use the Docker image in `docker-compose.yml`; do not fetch or
  compile the openGauss source tree by default.
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

1. Confirm or repair local openGauss build environment.
2. Review FDW callback definitions and contrib implementations.
3. Record team-provided Iceberg metadata, index scan, and delta write APIs when
   available.
4. Implement a minimal Iceberg FDW extension skeleton.
5. Add index scan planning/execution integration.
6. Add DML callbacks and transaction integration for delta writes.
