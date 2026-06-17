#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
COMPOSE_FILE="${ROOT_DIR}/docker-compose.yml"
CONTAINER_NAME="opengauss-iceberg-fdw"
SERVICE_NAME="opengauss"
GSQL_HOST="${GSQL_HOST:-/tmp}"
if [[ -z "${GSQL_HOST}" || "${GSQL_HOST}" == "Unknown" ]]; then
    GSQL_HOST="/tmp"
fi
GSQL_ENV="env -u PGHOST -u PGPORT -u PGSERVICE -u PGDATABASE -u PGUSER"
GSQL_BASE="${GSQL_ENV} gsql -h ${GSQL_HOST} -p 5432"

compose() {
    docker compose -f "${COMPOSE_FILE}" "$@"
}

usage() {
    cat <<'EOF'
Usage: tools/opengauss-docker.sh <command>

Commands:
  up       Pull image if needed and start openGauss in the background
  down     Stop and remove the openGauss container, keeping docker-data/
  restart  Restart the openGauss container
  status   Show compose status and one-shot container resource usage
  logs     Follow openGauss container logs
  check    Run select version() through gsql inside the container
  gsql     Open an interactive gsql session inside the container
EOF
}

require_docker() {
    if ! command -v docker >/dev/null 2>&1; then
        echo "docker is not installed or not in PATH" >&2
        exit 1
    fi

    if ! docker compose version >/dev/null 2>&1; then
        echo "docker compose v2 is required; install the Docker Compose plugin" >&2
        exit 1
    fi
}

run_gsql() {
    local sql="${1:-}"

    if [[ -n "${sql}" ]]; then
        docker exec "${CONTAINER_NAME}" bash -lc \
            "su - omm -c '${GSQL_BASE} -d postgres -c \"${sql}\"'"
    else
        docker exec -it "${CONTAINER_NAME}" bash -lc \
            "su - omm -c 'if command -v rlwrap >/dev/null 2>&1; then exec rlwrap ${GSQL_BASE} -d postgres; else exec ${GSQL_BASE} -d postgres; fi'"
    fi
}

main() {
    local command="${1:-}"

    case "${command}" in
        up)
            require_docker
            compose up -d
            ;;
        down)
            require_docker
            compose down
            ;;
        restart)
            require_docker
            compose restart "${SERVICE_NAME}"
            ;;
        status)
            require_docker
            compose ps
            docker stats --no-stream "${CONTAINER_NAME}" || true
            ;;
        logs)
            require_docker
            compose logs -f "${SERVICE_NAME}"
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
