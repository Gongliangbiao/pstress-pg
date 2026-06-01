#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
SCRIPT="${ROOT_DIR}/pstress/randomize_pstress_pg.sh"
TMPDIR="$(mktemp -d)"
trap 'rm -rf "${TMPDIR}"' EXIT

HELP_FILE="${TMPDIR}/help.txt"
cat > "${HELP_FILE}" <<'EOF'
--seed: Initial seed used for the test
 default#: 1

--threads: The number of threads to use
 default#: 1

--tables: Number of initial tables
 default#: 10

--queries-per-thread: The number of queries per thread
 default#: 1

--trx-prob-k: probability(out of 1000) of combining sql as single trx
 default#: 1

--select-with-join: Select rows using metadata-driven INNER JOIN between compatible tables
 default#: 40

--no-delete: do not execute any type of delete on tables
 default: 0
EOF

BIN="${TMPDIR}/fake-pstress-pg"
cat > "${BIN}" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail
if [[ "${1:-}" == "--help" && "${2:-}" == "--verbose" ]]; then
  cat "${PSTRESS_HELP_FILE}"
  exit 0
fi
printf '%s\n' "$*" > "${PSTRESS_INVOKE_LOG}"
exit 0
EOF
chmod +x "${BIN}"

CHECK="${TMPDIR}/check-ok.sh"
cat > "${CHECK}" <<'EOF'
#!/usr/bin/env bash
exit 0
EOF
chmod +x "${CHECK}"

cat > "${TMPDIR}/pg_isready" <<'EOF'
#!/usr/bin/env bash
exit 0
EOF
chmod +x "${TMPDIR}/pg_isready"

LOGDIR="${TMPDIR}/logs"
mkdir -p "${LOGDIR}"

PATH="${TMPDIR}:$PATH" \
PSTRESS_HELP_FILE="${HELP_FILE}" \
PSTRESS_INVOKE_LOG="${TMPDIR}/invoke.log" \
bash "${SCRIPT}" \
  --pstress-bin "${BIN}" \
  --pg-host 127.0.0.1 \
  --pg-port 5432 \
  --pg-user postgres \
  --pg-db postgres \
  --logdir "${LOGDIR}" \
  --round-seconds 1 \
  --max-rounds 1

test -f "${LOGDIR}/127.0.0.1-round-0001.cmd"
test -f "${LOGDIR}/127.0.0.1-round-0001.params"
test -f "${LOGDIR}/run-summary.log"
grep -q -- '--threads=' "${LOGDIR}/127.0.0.1-round-0001.cmd"
grep -q -- '--tables=' "${LOGDIR}/127.0.0.1-round-0001.cmd"
grep -q -- '--queries-per-thread=' "${LOGDIR}/127.0.0.1-round-0001.cmd"
grep -q -- '--seed=' "${LOGDIR}/127.0.0.1-round-0001.cmd"
! grep -q -- '--prepare' "${LOGDIR}/127.0.0.1-round-0001.cmd"
! grep -q -- '--metadata-path=' "${LOGDIR}/127.0.0.1-round-0001.cmd"
! grep -q -- '--step=' "${LOGDIR}/127.0.0.1-round-0001.cmd"
grep -q 'round=1' "${LOGDIR}/run-summary.log"
grep -q 'exit_code=0' "${LOGDIR}/run-summary.log"
grep -q 'first_failed_thread=none' "${LOGDIR}/run-summary.log"
grep -Eq '^(--no-delete|--select-with-join|--trx-prob-k|--threads|--tables|--queries-per-thread|--seed)=' "${LOGDIR}/127.0.0.1-round-0001.params"

BIN_FAIL="${TMPDIR}/fake-pstress-pg-fail"
cat > "${BIN_FAIL}" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail
if [[ "${1:-}" == "--help" && "${2:-}" == "--verbose" ]]; then
  cat "${PSTRESS_HELP_FILE}"
  exit 0
fi
logdir=""
step="1"
for arg in "$@"; do
  case "$arg" in
    --logdir=*) logdir="${arg#*=}" ;;
    --step=*) step="${arg#*=}" ;;
  esac
done
if [[ -n "$logdir" ]]; then
  mkdir -p "$logdir"
  printf '%s\n' '2026-06-01T00:00:01 100=>9ms  F SELECT broken' \
    'Error code 57P01 message terminating connection due to administrator command' \
    > "${logdir}/default.node.tld_step_${step}_thread-7.sql"
fi
exit 9
EOF
chmod +x "${BIN_FAIL}"

LOGDIR_FAIL="${TMPDIR}/logs-fail"
mkdir -p "${LOGDIR_FAIL}"
PATH="${TMPDIR}:$PATH" \
PSTRESS_HELP_FILE="${HELP_FILE}" \
bash "${SCRIPT}" \
  --pstress-bin "${BIN_FAIL}" \
  --pg-host 127.0.0.1 \
  --pg-port 5432 \
  --pg-user postgres \
  --pg-db postgres \
  --logdir "${LOGDIR_FAIL}" \
  --round-seconds 1 \
  --max-rounds 3

test -f "${LOGDIR_FAIL}/127.0.0.1-round-0001.cmd"
test ! -f "${LOGDIR_FAIL}/127.0.0.1-round-0002.cmd"
grep -q 'exit_code=9' "${LOGDIR_FAIL}/run-summary.log"
grep -q 'status=stop' "${LOGDIR_FAIL}/run-summary.log"
grep -q 'first_failed_thread=thread-7' "${LOGDIR_FAIL}/run-summary.log"

BIN_PREPARE="${TMPDIR}/fake-pstress-pg-prepare"
cat > "${BIN_PREPARE}" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail
if [[ "${1:-}" == "--help" && "${2:-}" == "--verbose" ]]; then
  cat "${PSTRESS_HELP_FILE_PREPARE}"
  exit 0
fi
printf '%s\n' "$*" > "${PSTRESS_INVOKE_LOG_PREPARE}"
exit 0
EOF
chmod +x "${BIN_PREPARE}"

HELP_FILE_PREPARE="${TMPDIR}/help-prepare.txt"
cat > "${HELP_FILE_PREPARE}" <<'EOF'
--seed: Initial seed used for the test
 default#: 1

--prepare: create new random tables and insert initial records
 default: 0

--threads: The number of threads to use
 default#: 1

--tables: Number of initial tables
 default#: 10

--queries-per-thread: The number of queries per thread
 default#: 1

--step: current step in pstress script
 default#: 1

--metadata-path: path of metadata file
 default:
EOF

LOGDIR_PREPARE="${TMPDIR}/logs-prepare"
mkdir -p "${LOGDIR_PREPARE}"
PATH="${TMPDIR}:$PATH" \
PSTRESS_FORCE_PREPARE=1 \
PSTRESS_HELP_FILE_PREPARE="${HELP_FILE_PREPARE}" \
PSTRESS_INVOKE_LOG_PREPARE="${TMPDIR}/invoke-prepare.log" \
bash "${SCRIPT}" \
  --pstress-bin "${BIN_PREPARE}" \
  --pg-host 127.0.0.1 \
  --pg-port 5432 \
  --pg-user postgres \
  --pg-db postgres \
  --logdir "${LOGDIR_PREPARE}" \
  --round-seconds 1 \
  --max-rounds 1

test -f "${LOGDIR_PREPARE}/127.0.0.1-round-0001.prepare.cmd"
test -f "${LOGDIR_PREPARE}/127.0.0.1-round-0001.run.cmd"
grep -q -- '--prepare' "${LOGDIR_PREPARE}/127.0.0.1-round-0001.prepare.cmd"
grep -q -- '--metadata-path=' "${LOGDIR_PREPARE}/127.0.0.1-round-0001.prepare.cmd"
! grep -q -- '--step=2' "${LOGDIR_PREPARE}/127.0.0.1-round-0001.prepare.cmd"
! grep -q -- '--prepare' "${LOGDIR_PREPARE}/127.0.0.1-round-0001.run.cmd"
grep -q -- '--metadata-path=' "${LOGDIR_PREPARE}/127.0.0.1-round-0001.run.cmd"
grep -q -- '--step=2' "${LOGDIR_PREPARE}/127.0.0.1-round-0001.run.cmd"
grep -q 'phase=prepare' "${LOGDIR_PREPARE}/run-summary.log"
grep -q 'phase=run' "${LOGDIR_PREPARE}/run-summary.log"
