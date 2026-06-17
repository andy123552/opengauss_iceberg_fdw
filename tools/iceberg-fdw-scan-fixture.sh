#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
STACK_SCRIPT="${ROOT_DIR}/tools/iceberg-fdw-stack.sh"
BRIDGE_SRC_DIR="${BRIDGE_SRC_DIR:-${ROOT_DIR}/../iceberg-rust-bridge}"
CONTAINER_NAME="${CONTAINER_NAME:-opengauss-iceberg-fdw}"
WAREHOUSE_DIR="${WAREHOUSE_DIR:-/var/lib/opengauss/data/tmp/iceberg_fdw_regress}"
SCAN_DB="${SCAN_DB:-iceberg_fdw_scan}"

require_docker() {
    if ! command -v docker >/dev/null 2>&1; then
        echo "docker is not installed or not in PATH" >&2
        exit 1
    fi
}

fixture_exists() {
    docker exec "${CONTAINER_NAME}" bash -lc "
        find '${WAREHOUSE_DIR}/scan_ns/scan_table' -type f \\( -name '*.metadata.json' -o -name '*.parquet' \\) 2>/dev/null | head -n 1
    " >/dev/null 2>&1
}

build_fixture_if_needed() {
    if fixture_exists; then
        echo "scan fixture already exists under ${WAREHOUSE_DIR}, skip rebuild"
        return
    fi

    echo "scan fixture is missing, building with iceberg-rust-bridge"
    (cd "${BRIDGE_SRC_DIR}" && cargo test --test file_scan_roundtrip file_scan_roundtrip -- --nocapture)
}

run_validation_sql() {
    docker exec "${CONTAINER_NAME}" bash -lc "
        set -e
        export GAUSSHOME=/usr/local/opengauss
        export LD_LIBRARY_PATH=/usr/local/opengauss/lib:\$LD_LIBRARY_PATH
        su - omm -c \"gsql -d postgres -p 5432 -c \\\"DROP DATABASE IF EXISTS ${SCAN_DB};\\\"\" >/dev/null
        su - omm -c \"gsql -d postgres -p 5432 -c \\\"CREATE DATABASE ${SCAN_DB};\\\"\" >/dev/null
        su - omm -c \"gsql -d ${SCAN_DB} -p 5432 -v ON_ERROR_STOP=1 -f /tmp/iceberg_fdw_build/sql/managed_scan.sql\"
    "
}

main() {
    require_docker
    bash "${STACK_SCRIPT}" setup
    build_fixture_if_needed
    run_validation_sql
}

main "$@"
