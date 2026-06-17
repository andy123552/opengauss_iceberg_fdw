#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DOCKER_TOOL="${ROOT_DIR}/tools/opengauss-docker.sh"
FDW_STACK_TOOL="${ROOT_DIR}/tools/iceberg-fdw-stack.sh"
CONTAINER_NAME="opengauss-iceberg-fdw"
PG_CONFIG="/usr/local/opengauss/bin/pg_config"
REMOTE_WORK_ROOT="/tmp/iceberg_stack_build"
HOST_WORK_ROOT="${ICEBERG_STACK_WORK_ROOT:-$(mktemp -d /tmp/iceberg_stack_build.XXXXXX)}"
CATALOG_SRC_DIR="${CATALOG_SRC_DIR:-${ROOT_DIR}/Catalog}"
FDW_SRC_DIR="${FDW_SRC_DIR:-${ROOT_DIR}/iceberg_fdw}"
BRIDGE_SRC_DIR="${BRIDGE_SRC_DIR:-${ROOT_DIR}/iceberg-rust-bridge}"
OG_INCLUDE_SRC_DIR="${OG_INCLUDE_SRC_DIR:-${ROOT_DIR}/openGauss-server/src/include}"
BRIDGE_SO_IN_CONTAINER="${BRIDGE_SO_IN_CONTAINER:-/usr/local/opengauss/lib/postgresql/libiceberg_rust_bridge.so}"

cleanup() {
    if [[ -z "${ICEBERG_STACK_WORK_ROOT:-}" ]]; then
        rm -rf "${HOST_WORK_ROOT}"
    fi
}
trap cleanup EXIT

usage() {
    cat <<'EOF'
Usage: tools/iceberg-stack.sh <command>

Commands:
  full      Start Docker openGauss and build/install bridge, Catalog, and iceberg_fdw
  bridge    Build bridge and copy libiceberg_rust_bridge.so into the container
  catalog   Build and install Catalog into the container
  fdw       Build and install iceberg_fdw into the container
  up        Start the openGauss container
  restart   Restart the openGauss container
  check     Verify gsql connectivity
  doctor    Print Docker, socket, environment, and gsql diagnostics
  gsql      Open an interactive gsql session
  status    Show container status
  down      Stop the openGauss container

Environment overrides:
  CATALOG_SRC_DIR
  FDW_SRC_DIR
  BRIDGE_SRC_DIR
  OG_INCLUDE_SRC_DIR
  ICEBERG_STACK_WORK_ROOT
  BRIDGE_SO_IN_CONTAINER
EOF
}

require_file() {
    if [[ ! -f "$1" ]]; then
        echo "missing required file: $1" >&2
        exit 1
    fi
}

require_dir() {
    if [[ ! -d "$1" ]]; then
        echo "missing required directory: $1" >&2
        exit 1
    fi
}

require_host_tools() {
    command -v docker >/dev/null 2>&1 || {
        echo "docker is not installed or not in PATH" >&2
        exit 1
    }
    command -v cargo >/dev/null 2>&1 || {
        echo "cargo is not installed or not in PATH" >&2
        exit 1
    }
}

run_docker_tool() {
    bash "${DOCKER_TOOL}" "$@"
}

run_fdw_stack_tool() {
    bash "${FDW_STACK_TOOL}" "$@"
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
    local staged_dir="${HOST_WORK_ROOT}/openGauss-src-include"
    stage_dir_to_container "${OG_INCLUDE_SRC_DIR}" "/tmp/openGauss-src-include" "${staged_dir}"
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

build_and_install_catalog() {
    require_dir "${CATALOG_SRC_DIR}"
    prepare_openGauss_include

    local staged_dir="${HOST_WORK_ROOT}/Catalog"
    local remote_dir="${REMOTE_WORK_ROOT}/Catalog"
    stage_dir_to_container "${CATALOG_SRC_DIR}" "${remote_dir}" "${staged_dir}"
    patch_catalog_makefile "${remote_dir}"

    docker exec "${CONTAINER_NAME}" sh -lc "
        set -e
        mkdir -p /tmp/ogsrc/src
        ln -sfn /tmp/openGauss-src-include /tmp/ogsrc/src/include
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

build_and_install_fdw() {
    require_dir "${FDW_SRC_DIR}"
    prepare_openGauss_include

    local staged_dir="${HOST_WORK_ROOT}/iceberg_fdw"
    local remote_dir="${REMOTE_WORK_ROOT}/iceberg_fdw"
    stage_dir_to_container "${FDW_SRC_DIR}" "${remote_dir}" "${staged_dir}"

    docker exec "${CONTAINER_NAME}" sh -lc "
        set -e
        cd '${remote_dir}'
        export GAUSSHOME=/usr/local/opengauss
        export LD_LIBRARY_PATH=/usr/local/opengauss/lib:\$LD_LIBRARY_PATH
        MAKEFLAGS=-j2 make PG_CONFIG='${PG_CONFIG}'
        make install PG_CONFIG='${PG_CONFIG}'
    "
}

build_and_install_bridge() {
    require_dir "${BRIDGE_SRC_DIR}"
    require_file "${BRIDGE_SRC_DIR}/Cargo.toml"

    (cd "${BRIDGE_SRC_DIR}" && cargo build --release)

    docker exec "${CONTAINER_NAME}" sh -lc "mkdir -p '$(dirname "${BRIDGE_SO_IN_CONTAINER}")'"
    docker cp "${BRIDGE_SRC_DIR}/target/release/libiceberg_rust_bridge.so" "${CONTAINER_NAME}:${BRIDGE_SO_IN_CONTAINER}"
    docker exec "${CONTAINER_NAME}" sh -lc "chown omm:omm '${BRIDGE_SO_IN_CONTAINER}'"
}

restart_container() {
    run_docker_tool restart
}

full_build() {
    run_fdw_stack_tool setup
}

main() {
    local command="${1:-}"

    case "${command}" in
        full)
            require_host_tools
            full_build
            ;;
        bridge)
            require_host_tools
            run_fdw_stack_tool bridge
            ;;
        catalog)
            require_host_tools
            run_fdw_stack_tool catalog
            ;;
        fdw)
            require_host_tools
            run_fdw_stack_tool fdw
            ;;
        restart|check|gsql|doctor)
            run_fdw_stack_tool "${command}"
            ;;
        up|status|down)
            run_docker_tool "${command}"
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
