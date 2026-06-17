#!/usr/bin/env bash
set -euo pipefail

usage() {
    cat <<'EOF'
Usage: pg_regress [options]

Supported options:
  --dbname=DB
  --inputdir=DIR
  --outputdir=DIR
  --schedule=FILE
  --psqldir=DIR
  --port=PORT
  --host=HOST
  --use-existing

This is a lightweight regression harness for the managed Iceberg FDW tests.
It runs each test in a fresh database and diffs SQL output against expected.
EOF
}

dbname="iceberg_fdw_regress"
inputdir="."
outputdir="."
schedule=""
psqldir="/usr/local/opengauss/bin"
host="/tmp"
port="5432"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --dbname=*)
            dbname="${1#*=}"
            shift
            ;;
        --dbname)
            dbname="$2"
            shift 2
            ;;
        --inputdir=*)
            inputdir="${1#*=}"
            shift
            ;;
        --inputdir)
            inputdir="$2"
            shift 2
            ;;
        --outputdir=*)
            outputdir="${1#*=}"
            shift
            ;;
        --outputdir)
            outputdir="$2"
            shift 2
            ;;
        --schedule=*)
            schedule="${1#*=}"
            shift
            ;;
        --schedule)
            schedule="$2"
            shift 2
            ;;
        --psqldir=*)
            psqldir="${1#*=}"
            shift
            ;;
        --psqldir)
            psqldir="$2"
            shift 2
            ;;
        --host=*)
            host="${1#*=}"
            shift
            ;;
        --host)
            host="$2"
            shift 2
            ;;
        --port=*)
            port="${1#*=}"
            shift
            ;;
        --port)
            port="$2"
            shift 2
            ;;
        --use-existing)
            shift
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            shift
            ;;
    esac
done

if [[ -z "${schedule}" ]]; then
    echo "pg_regress: --schedule is required" >&2
    exit 1
fi

if [[ ! -f "${schedule}" ]]; then
    echo "pg_regress: schedule file not found: ${schedule}" >&2
    exit 1
fi

if [[ -z "${host}" || "${host}" == "Unknown" ]]; then
    host="/tmp"
fi

results_dir="${outputdir%/}/results"
mkdir -p "${results_dir}"

tests=()
while IFS= read -r line; do
    line="${line%%#*}"
    line="${line#"${line%%[![:space:]]*}"}"
    line="${line%"${line##*[![:space:]]}"}"
    [[ -z "${line}" ]] && continue
    if [[ "${line}" =~ ^test:[[:space:]]*([A-Za-z0-9_.-]+)$ ]]; then
        tests+=("${BASH_REMATCH[1]}")
    elif [[ "${line}" =~ ^([A-Za-z0-9_.-]+)$ ]]; then
        tests+=("${BASH_REMATCH[1]}")
    fi
done < "${schedule}"

if [[ ${#tests[@]} -eq 0 ]]; then
    echo "pg_regress: no tests listed in schedule ${schedule}" >&2
    exit 1
fi

sql_exec() {
    local db="$1"
    local sql="$2"
    local out="$3"
    local sql_escaped
    sql_escaped=$(printf '%q' "${sql}")
    su - omm -c "bash -lc 'export GAUSSHOME=/usr/local/opengauss; export LD_LIBRARY_PATH=/usr/local/opengauss/lib:\${LD_LIBRARY_PATH}; env -u PGHOST -u PGPORT -u PGSERVICE -u PGDATABASE -u PGUSER \"${psqldir}/gsql\" -X -a -q -v ON_ERROR_STOP=1 -h \"${host}\" -p ${port} -d \"${db}\" -f \"${sql_escaped}\"'" >"${out}" 2>&1
}

compare_files() {
    local expect="$1"
    local actual="$2"
    if ! diff -u "${expect}" "${actual}"; then
        return 1
    fi
}

passed=0
failed=0

for test in "${tests[@]}"; do
    sql_file="${inputdir%/}/sql/${test}.sql"
    expect_file="${inputdir%/}/expected/${test}.out"
    result_file="${results_dir}/${test}.out"

    if [[ ! -f "${sql_file}" ]]; then
        echo "pg_regress: missing SQL file ${sql_file}" >&2
        exit 1
    fi
    if [[ ! -f "${expect_file}" ]]; then
        echo "pg_regress: missing expected file ${expect_file}" >&2
        exit 1
    fi

    su - omm -c "bash -lc 'export GAUSSHOME=/usr/local/opengauss; export LD_LIBRARY_PATH=/usr/local/opengauss/lib:\${LD_LIBRARY_PATH}; env -u PGHOST -u PGPORT -u PGSERVICE -u PGDATABASE -u PGUSER \"${psqldir}/gsql\" -X -h \"${host}\" -p ${port} -d postgres -c \"DROP DATABASE IF EXISTS \\\"${dbname}\\\";\" >/dev/null 2>&1 || true'"
    su - omm -c "bash -lc 'export GAUSSHOME=/usr/local/opengauss; export LD_LIBRARY_PATH=/usr/local/opengauss/lib:\${LD_LIBRARY_PATH}; env -u PGHOST -u PGPORT -u PGSERVICE -u PGDATABASE -u PGUSER \"${psqldir}/gsql\" -X -h \"${host}\" -p ${port} -d postgres -c \"CREATE DATABASE \\\"${dbname}\\\";\" >/dev/null'"

    if sql_exec "${dbname}" "${sql_file}" "${result_file}"; then
        if compare_files "${expect_file}" "${result_file}"; then
            passed=$((passed + 1))
            continue
        fi
    fi

    failed=$((failed + 1))
    echo "pg_regress: failed test ${test}" >&2
    echo "  sql: ${sql_file}" >&2
    echo "  expected: ${expect_file}" >&2
    echo "  actual: ${result_file}" >&2
    compare_files "${expect_file}" "${result_file}" || true
    break
done

if [[ ${failed} -eq 0 ]]; then
    echo " All ${passed} tests passed. "
    exit 0
fi

echo " ${passed} of ${#tests[@]} tests passed. "
exit 1
