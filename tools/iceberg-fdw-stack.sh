#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
COMPOSE_FILE="${COMPOSE_FILE:-${ROOT_DIR}/docker-compose.yml}"
SERVICE_NAME="${SERVICE_NAME:-opengauss}"
CONTAINER_NAME="${CONTAINER_NAME:-opengauss-iceberg-fdw}"
PG_CONFIG="${PG_CONFIG:-/usr/local/opengauss/bin/pg_config}"
REMOTE_WORK_ROOT="${REMOTE_WORK_ROOT:-/tmp/iceberg_fdw_build}"
HOST_WORK_ROOT="${ICEBERG_FDW_WORK_ROOT:-$(mktemp -d /tmp/iceberg_fdw_build.XXXXXX)}"
FDW_SRC_DIR="${FDW_SRC_DIR:-${ROOT_DIR}/iceberg_fdw}"
BRIDGE_SRC_DIR="${BRIDGE_SRC_DIR:-${ROOT_DIR}/iceberg-rust-bridge}"
CATALOG_SRC_DIR="${CATALOG_SRC_DIR:-${ROOT_DIR}/Catalog}"
OG_INCLUDE_SRC_DIR="${OG_INCLUDE_SRC_DIR:-${ROOT_DIR}/openGauss-server/src/include}"
BRIDGE_SO_IN_CONTAINER="${BRIDGE_SO_IN_CONTAINER:-/usr/local/opengauss/lib/postgresql/libiceberg_rust_bridge.so}"

cleanup() {
    if [[ -z "${ICEBERG_FDW_WORK_ROOT:-}" ]]; then
        rm -rf "${HOST_WORK_ROOT}"
    fi
}
trap cleanup EXIT

usage() {
    cat <<'EOF'
Usage: tools/iceberg-fdw-stack.sh <command>

Commands:
  setup     Build bridge, Catalog, and iceberg_fdw, then restart the container
  full      Build bridge, Catalog, and iceberg_fdw, then restart the container
  bridge    Build bridge and copy libiceberg_rust_bridge.so into the container
  catalog   Build and install Catalog into the container
  fdw       Build and install iceberg_fdw into the container
  test      Run managed DDL and managed scan SQL in the container
  restart   Restart the openGauss container
  check     Run a gsql version probe
  gsql      Open an interactive gsql session

Environment overrides:
  CONTAINER_NAME
  PG_CONFIG
  FDW_SRC_DIR
  BRIDGE_SRC_DIR
  CATALOG_SRC_DIR
  OG_INCLUDE_SRC_DIR
  ICEBERG_FDW_WORK_ROOT
  REMOTE_WORK_ROOT
  BRIDGE_SO_IN_CONTAINER
  GSQL_WAIT_TIMEOUT
EOF
}

require_dir() {
    if [[ ! -d "$1" ]]; then
        echo "missing required directory: $1" >&2
        exit 1
    fi
}

require_file() {
    if [[ ! -f "$1" ]]; then
        echo "missing required file: $1" >&2
        exit 1
    fi
}

require_docker() {
    if ! command -v docker >/dev/null 2>&1; then
        echo "docker is not installed or not in PATH" >&2
        exit 1
    fi
}

ensure_container_running() {
    if docker inspect -f '{{.State.Running}}' "${CONTAINER_NAME}" >/dev/null 2>&1; then
        if [[ "$(docker inspect -f '{{.State.Running}}' "${CONTAINER_NAME}")" == "true" ]]; then
            return
        fi
    fi

    if [[ -f "${COMPOSE_FILE}" ]] && docker compose version >/dev/null 2>&1; then
        docker compose -f "${COMPOSE_FILE}" up -d "${SERVICE_NAME}"
    else
        echo "container ${CONTAINER_NAME} is not running; start it before running this command" >&2
        exit 1
    fi
}

wait_for_gsql() {
    local timeout="${GSQL_WAIT_TIMEOUT:-60}"
    local start="${SECONDS}"

    until docker exec "${CONTAINER_NAME}" bash -lc \
        "su - omm -c 'gsql -d postgres -p 5432 -At -c \"select 1;\" >/dev/null'" >/dev/null 2>&1; do
        if (( SECONDS - start >= timeout )); then
            echo "timed out waiting for openGauss gsql connectivity in ${CONTAINER_NAME}" >&2
            docker ps --filter "name=${CONTAINER_NAME}" --format 'table {{.Names}}\t{{.Status}}\t{{.Ports}}' >&2 || true
            docker logs --tail 80 "${CONTAINER_NAME}" >&2 || true
            exit 1
        fi
        sleep 1
    done
}

run_gsql() {
    local sql="${1:-}"
    ensure_container_running
    wait_for_gsql

    if [[ -n "${sql}" ]]; then
        docker exec "${CONTAINER_NAME}" bash -lc \
            "su - omm -c 'gsql -d postgres -p 5432 -c \"${sql}\"'"
    else
        docker exec -it "${CONTAINER_NAME}" bash -lc \
            "su - omm -c 'if command -v rlwrap >/dev/null 2>&1; then exec rlwrap gsql -d postgres -p 5432; else exec gsql -d postgres -p 5432; fi'"
    fi
}

stage_dir_to_container() {
    local src_dir="$1"
    local remote_dir="$2"
    local staged_dir="$3"

    rm -rf "${staged_dir}"
    mkdir -p "${staged_dir}"
    cp -a "${src_dir}/." "${staged_dir}/"
    docker exec "${CONTAINER_NAME}" sh -lc "rm -rf '${remote_dir}' && mkdir -p '${remote_dir}'"
    docker cp "${staged_dir}/." "${CONTAINER_NAME}:${remote_dir}"
}

prepare_openGauss_include() {
    if [[ -d "${OG_INCLUDE_SRC_DIR}" ]]; then
        local staged_dir="${HOST_WORK_ROOT}/openGauss-src-include"
        stage_dir_to_container "${OG_INCLUDE_SRC_DIR}" "/tmp/openGauss-src-include" "${staged_dir}"
    fi
}

patch_catalog_makefile() {
    local remote_dir="$1"
    docker exec "${CONTAINER_NAME}" sh -lc "
        set -e
        cd '${remote_dir}'
        if ! grep -q 'exclude_option = -fPIE' Makefile; then
            cat <<'EOF' >> Makefile

exclude_option = -fPIE
override CPPFLAGS := \$(filter-out \$(exclude_option),\$(CPPFLAGS))
override CFLAGS := \$(filter-out \$(exclude_option),\$(CFLAGS))
override CXXFLAGS := \$(filter-out \$(exclude_option),\$(CXXFLAGS))
EOF
        fi
    "
}

build_bridge() {
    require_dir "${BRIDGE_SRC_DIR}"
    require_file "${BRIDGE_SRC_DIR}/Cargo.toml"
    (cd "${BRIDGE_SRC_DIR}" && cargo build --release)
    docker exec "${CONTAINER_NAME}" sh -lc "mkdir -p '$(dirname "${BRIDGE_SO_IN_CONTAINER}")'"
    docker cp "${BRIDGE_SRC_DIR}/target/release/libiceberg_rust_bridge.so" "${CONTAINER_NAME}:${BRIDGE_SO_IN_CONTAINER}"
    docker exec "${CONTAINER_NAME}" sh -lc "chown omm:omm '${BRIDGE_SO_IN_CONTAINER}'"
}

build_catalog() {
    require_dir "${CATALOG_SRC_DIR}"
    prepare_openGauss_include

    local staged_dir="${HOST_WORK_ROOT}/Catalog"
    local remote_dir="${REMOTE_WORK_ROOT}/Catalog"
    stage_dir_to_container "${CATALOG_SRC_DIR}" "${remote_dir}" "${staged_dir}"
    patch_catalog_makefile "${remote_dir}"

    docker exec "${CONTAINER_NAME}" sh -lc "
        set -e
        mkdir -p /tmp/ogsrc/src
        if [[ -d /tmp/openGauss-src-include ]]; then
            ln -sfn /tmp/openGauss-src-include /tmp/ogsrc/src/include
        fi
        cd '${remote_dir}'
        export GAUSSHOME=/usr/local/opengauss
        export LD_LIBRARY_PATH=/usr/local/opengauss/lib:\$LD_LIBRARY_PATH
        export GAUSS_SRC=/tmp/ogsrc
        MAKEFLAGS=-j2 make PG_CONFIG='${PG_CONFIG}'
        make install PG_CONFIG='${PG_CONFIG}'
        cp /usr/local/opengauss/lib/postgresql/iceberg_catalog.so \
           /usr/local/opengauss/lib/postgresql/proc_srclib/iceberg_catalog
        chown omm:omm /usr/local/opengauss/lib/postgresql/proc_srclib/iceberg_catalog
    "
}

build_fdw() {
    require_dir "${FDW_SRC_DIR}"
    prepare_openGauss_include

    local staged_dir="${HOST_WORK_ROOT}/iceberg_fdw"
    local remote_dir="${REMOTE_WORK_ROOT}/iceberg_fdw"
    stage_dir_to_container "${FDW_SRC_DIR}" "${remote_dir}" "${staged_dir}"

    docker exec "${CONTAINER_NAME}" sh -lc "
        set -e
        if [[ -d /tmp/openGauss-src-include ]]; then
            mkdir -p /tmp/ogsrc/src
            ln -sfn /tmp/openGauss-src-include /tmp/ogsrc/src/include
        fi
        cd '${remote_dir}'
        export GAUSSHOME=/usr/local/opengauss
        export LD_LIBRARY_PATH=/usr/local/opengauss/lib:\$LD_LIBRARY_PATH
        MAKEFLAGS=-j2 make PG_CONFIG='${PG_CONFIG}'
        make install PG_CONFIG='${PG_CONFIG}'
    "
}

restart_container() {
    docker restart "${CONTAINER_NAME}" >/dev/null
    wait_for_gsql
}

run_tests() {
    ensure_container_running
    wait_for_gsql
    docker exec "${CONTAINER_NAME}" bash -lc 'mkdir -p /var/lib/opengauss/data/tmp/iceberg_fdw_regress'
    docker exec "${CONTAINER_NAME}" sh -lc "
        set -e
        su - omm -c \"gsql -d postgres -p 5432 -c \\\"DROP DATABASE IF EXISTS iceberg_fdw_regress;\\\"\"
        su - omm -c \"gsql -d postgres -p 5432 -c \\\"CREATE DATABASE iceberg_fdw_regress;\\\"\"
        su - omm -c \"gsql -d iceberg_fdw_regress -p 5432 -v ON_ERROR_STOP=1 -f '${REMOTE_WORK_ROOT}/iceberg_fdw/sql/managed_ddl.sql'\"
        su - omm -c \"gsql -d iceberg_fdw_regress -p 5432 -v ON_ERROR_STOP=1 -f '${REMOTE_WORK_ROOT}/iceberg_fdw/sql/managed_scan.sql'\"
    "
}

full_build() {
    ensure_container_running
    wait_for_gsql
    build_bridge
    build_catalog
    build_fdw
    restart_container
    run_tests
}

setup_build() {
    ensure_container_running
    wait_for_gsql
    build_bridge
    build_catalog
    build_fdw
    restart_container
}

main() {
    local command="${1:-}"

    case "${command}" in
        setup)
            require_docker
            setup_build
            ;;
        full)
            require_docker
            full_build
            ;;
        bridge)
            require_docker
            ensure_container_running
            wait_for_gsql
            build_bridge
            restart_container
            ;;
        catalog)
            require_docker
            ensure_container_running
            wait_for_gsql
            build_catalog
            restart_container
            ;;
        fdw)
            require_docker
            ensure_container_running
            wait_for_gsql
            build_fdw
            restart_container
            ;;
        test)
            require_docker
            run_tests
            ;;
        restart)
            require_docker
            restart_container
            ;;
        check)
            require_docker
            run_gsql "select version();"
            ;;
        gsql)
            require_docker
            run_gsql
            ;;
        -h|--help|help|"")
            usage
            ;;
        *)
            echo "unknown command: ${command}" >&2
            usage >&2
            exit 1
            ;;
    esac
}

main "$@"
