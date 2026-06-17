#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
STACK_SCRIPT="${ROOT_DIR}/tools/iceberg-fdw-stack.sh"
CONTAINER_NAME="${CONTAINER_NAME:-opengauss-iceberg-fdw}"
REGRESS_BIN_IN_CONTAINER="${REGRESS_BIN_IN_CONTAINER:-/usr/local/opengauss/bin/pg_regress}"
REMOTE_WORK_ROOT="${REMOTE_WORK_ROOT:-/tmp/iceberg_fdw_build}"
GSQL_HOST="${GSQL_HOST:-/tmp}"
if [[ -z "${GSQL_HOST}" || "${GSQL_HOST}" == "Unknown" ]]; then
    GSQL_HOST="/tmp"
fi

require_docker() {
    if ! command -v docker >/dev/null 2>&1; then
        echo "docker is not installed or not in PATH" >&2
        exit 1
    fi
}

install_pg_regress() {
    docker cp "${ROOT_DIR}/tools/pg_regress_iceberg.sh" "${CONTAINER_NAME}:${REGRESS_BIN_IN_CONTAINER}"
    docker exec "${CONTAINER_NAME}" chmod +x "${REGRESS_BIN_IN_CONTAINER}"
}

prepare_schedule() {
    docker exec "${CONTAINER_NAME}" bash -lc "cat > '${REMOTE_WORK_ROOT}/iceberg_fdw.schedule' <<'EOF'
test: managed_ddl
test: managed_scan
EOF"
}

run_pg_regress() {
    docker exec "${CONTAINER_NAME}" bash -lc "
        export GAUSSHOME=/usr/local/opengauss
        export LD_LIBRARY_PATH=/usr/local/opengauss/lib:\$LD_LIBRARY_PATH
        '${REGRESS_BIN_IN_CONTAINER}' \
          --use-existing \
          --dbname=iceberg_fdw_regress \
          --inputdir='${REMOTE_WORK_ROOT}' \
          --outputdir='${REMOTE_WORK_ROOT}' \
          --schedule='${REMOTE_WORK_ROOT}/iceberg_fdw.schedule' \
          --psqldir=/usr/local/opengauss/bin \
          --host='${GSQL_HOST}' \
          --port=5432
    "
}

main() {
    require_docker
    bash "${STACK_SCRIPT}" setup
    install_pg_regress
    prepare_schedule
    run_pg_regress
}

main "$@"
