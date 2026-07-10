#!/usr/bin/env bash
if [ -z "${BASH_VERSION:-}" ]; then
  exec bash "$0" "$@"
fi

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

usage() {
  cat <<'EOF'
usage: randomize_pstress_pg.sh
  --pstress-bin <path>
  --pg-host <host>
  --pg-port <port>
  --pg-user <user>
  --pg-db <database>
  [--pg-password <password>]
  --logdir <dir>
  [--round-seconds <seconds>]
  [--max-rounds <count>]
  [--base-seed <seed>]

Runs repeated pstress-pg rounds with randomized non-connection parameters.
Each round writes command, parameters, and a summary row
into the shared log directory. The driver stops on non-zero pstress-pg exit
or when the PostgreSQL server check command fails.
EOF
}

die() {
  echo "ERROR: $*" >&2
  exit 1
}

require_cmd() {
  command -v "$1" >/dev/null 2>&1 || die "required command not found: $1"
}

trim() {
  local value="$1"
  value="${value#"${value%%[![:space:]]*}"}"
  value="${value%"${value##*[![:space:]]}"}"
  printf '%s' "$value"
}

rand_int_range() {
  local low="$1"
  local high="$2"
  if (( high <= low )); then
    printf '%s' "$low"
    return
  fi
  printf '%s' $(( low + RANDOM % (high - low + 1) ))
}

chance_percent() {
  local percent="$1"
  (( RANDOM % 100 < percent ))
}

add_param() {
  local name="$1"
  local value="$2"
  PARAM_LINES+=("--${name}=${value}")
  CMD_ARGS+=("--${name}=${value}")
}

upsert_param() {
  local name="$1"
  local value="$2"
  local needle="--${name}="
  local replacement="--${name}=${value}"
  local i
  for (( i = 0; i < ${#PARAM_LINES[@]}; i++ )); do
    if [[ "${PARAM_LINES[$i]}" == ${needle}* ]]; then
      PARAM_LINES[$i]="$replacement"
    fi
  done
  for (( i = 0; i < ${#CMD_ARGS[@]}; i++ )); do
    if [[ "${CMD_ARGS[$i]}" == ${needle}* ]]; then
      CMD_ARGS[$i]="$replacement"
    fi
  done
}

add_flag() {
  local name="$1"
  PARAM_LINES+=("--${name}=true")
  CMD_ARGS+=("--${name}")
}

next_arg_value() {
  local name="$1"
  local value="${2:-}"
  [[ -n "$value" ]] || die "${name} requires a value"
  printf '%s' "$value"
}

maybe_add_flag() {
  local name="$1"
  local percent="$2"
  if chance_percent "$percent"; then
    add_flag "$name"
    return 0
  fi
  return 1
}

join_by() {
  local sep="$1"
  shift
  local out=""
  local first=1
  local item
  for item in "$@"; do
    if (( first )); then
      out="$item"
      first=0
    else
      out+="${sep}${item}"
    fi
  done
  printf '%s' "$out"
}

shell_escape_array() {
  local out=()
  local item
  for item in "$@"; do
    out+=("$(printf '%q' "$item")")
  done
  join_by " " "${out[@]}"
}

safe_filename_component() {
  local value="$1"
  value="${value//[^A-Za-z0-9._-]/_}"
  if [[ -z "$value" ]]; then
    value="unknown"
  fi
  printf '%s' "$value"
}

get_option_default() {
  local wanted="$1"
  local i
  for (( i = 0; i < ${#OPTION_NAMES[@]}; i++ )); do
    if [[ "${OPTION_NAMES[$i]}" == "$wanted" ]]; then
      printf '%s' "${OPTION_DEFAULTS[$i]}"
      return 0
    fi
  done
  return 1
}

init_option_metadata() {
  OPTION_NAMES=()
  OPTION_KINDS=()
  OPTION_DEFAULTS=()
  BOOL_OPTIONS=()
  INT_OPTIONS=()
  STRING_OPTIONS=()

  local metadata_file
  metadata_file="$(mktemp "${TMPDIR:-/tmp}/pstress-pg-help.XXXXXX")"
  if ! "${pstress_bin}" --help --verbose > "$metadata_file"; then
    rm -f "$metadata_file"
    die "failed to read pstress option metadata from: ${pstress_bin}"
  fi

  local current=""
  local line
  while IFS= read -r line; do
    if [[ "$line" =~ ^--([a-z0-9-]+): ]]; then
      current="${BASH_REMATCH[1]}"
      continue
    fi
    [[ -n "$current" ]] || continue
    if [[ "$line" =~ default#:[[:space:]]*(.*)$ ]]; then
      OPTION_NAMES+=("$current")
      OPTION_KINDS+=("int")
      OPTION_DEFAULTS+=("$(trim "${BASH_REMATCH[1]}")")
      INT_OPTIONS+=("$current")
      current=""
    elif [[ "$line" =~ default:[[:space:]]*(.*)$ ]]; then
      OPTION_NAMES+=("$current")
      OPTION_DEFAULTS+=("$(trim "${BASH_REMATCH[1]}")")
      case "$(trim "${BASH_REMATCH[1]}")" in
        0|1)
          OPTION_KINDS+=("bool")
          BOOL_OPTIONS+=("$current")
          ;;
        *)
          OPTION_KINDS+=("string")
          STRING_OPTIONS+=("$current")
          ;;
      esac
      current=""
    fi
  done < "$metadata_file"

  rm -f "$metadata_file"
}

string_choice_for_option() {
  local option="$1"
  case "$option" in
    alter-algorithm)
      local choices=("all" "INPLACE" "COPY" "DEFAULT" "INPLACE,COPY" "COPY,DEFAULT")
      printf '%s' "${choices[$(( RANDOM % ${#choices[@]} ))]}"
      ;;
    alter-lock)
      local choices=("all" "DEFAULT" "NONE" "SHARED" "EXCLUSIVE" "NONE,SHARED" "SHARED,EXCLUSIVE")
      printf '%s' "${choices[$(( RANDOM % ${#choices[@]} ))]}"
      ;;
    partition-types)
      local choices=("all" "LIST" "HASH" "KEY" "RANGE" "LIST,HASH" "HASH,RANGE" "LIST,RANGE,KEY")
      printf '%s' "${choices[$(( RANDOM % ${#choices[@]} ))]}"
      ;;
    generated-column-kind)
      local choices=("random" "virtual" "stored")
      printf '%s' "${choices[$(( RANDOM % ${#choices[@]} ))]}"
      ;;
    pg18-copy-mode)
      local choices=("random" "from-stdin" "to-stdout" "query-to-stdout" "matview-to-stdout")
      printf '%s' "${choices[$(( RANDOM % ${#choices[@]} ))]}"
      ;;
    pg18-copy-log-verbosity)
      local choices=("random" "default" "verbose" "silent")
      printf '%s' "${choices[$(( RANDOM % ${#choices[@]} ))]}"
      ;;
    grammar-file)
      printf '%s' "${REPO_ROOT}/src/grammar.sql"
      ;;
    infile)
      printf '%s' "${REPO_ROOT}/src/pquery.sql"
      ;;
    *)
      printf '%s' "$(get_option_default "$option")"
      ;;
  esac
}

generic_int_value() {
  local option="$1"
  local default_value
  default_value="$(get_option_default "$option" || true)"
  default_value="${default_value:-1}"
  if ! [[ "$default_value" =~ ^[0-9]+$ ]]; then
    default_value=1
  fi
  local upper
  if (( default_value <= 1 )); then
    upper=10
  elif (( default_value <= 10 )); then
    upper=$(( default_value * 5 ))
  else
    upper=$(( default_value * 3 ))
  fi
  rand_int_range 0 "$upper"
}

emit_curated_int() {
  local option="$1"
  case "$option" in
    seed)
      add_param "$option" "$current_seed"
      ;;
    seconds)
      add_param "$option" "$round_seconds"
      ;;
    threads)
      add_param "$option" "$(rand_int_range 1 64)"
      ;;
    tables)
      add_param "$option" "$(rand_int_range 1 128)"
      ;;
    indexes)
      add_param "$option" "$(rand_int_range 1 16)"
      ;;
    columns)
      add_param "$option" "$(rand_int_range 1 32)"
      ;;
    index-columns)
      add_param "$option" "$(rand_int_range 1 8)"
      ;;
    records)
      add_param "$option" "$(rand_int_range 0 5000)"
      ;;
    queries-per-thread)
      add_param "$option" "$(rand_int_range 1 2000)"
      ;;
    trx-prob-k)
      add_param "$option" "$(rand_int_range 0 1000)"
      ;;
    trx-size)
      add_param "$option" "$(rand_int_range 1 64)"
      ;;
    commit-prob)
      add_param "$option" "$(rand_int_range 0 100)"
      ;;
    savepoint-prob-k)
      add_param "$option" "$(rand_int_range 0 1000)"
      ;;
    trx-ddl-prob-k)
      add_param "$option" "$(rand_int_range 0 1000)"
      ;;
    trx-ddl-size)
      add_param "$option" "$(rand_int_range 1 16)"
      ;;
    pk-prob|fk-prob|partition-prob|temporary-prob|unlogged-prob)
      add_param "$option" "$(rand_int_range 0 100)"
      ;;
    select-all-rows|select-single-row|select-with-join|select-with-cte|insert-row|update-with-cond|delete-all-rows|delete-with-cond)
      add_param "$option" "$(rand_int_range 0 1000)"
      ;;
    modify-column|check|add-drop-partition|drop-column|add-column|drop-index|add-index|rename-column|rename-index|optimize|analyze|truncate|recreate-table|grammar-sql)
      add_param "$option" "$(rand_int_range 0 50)"
      ;;
    vacuum|vacuum-full|checkpoint|create-index-concurrently|reindex|cluster-table|brin-expression-index|gist-index|create-matview|refresh-matview-concurrently|select-matview|drop-matview|prepared-tx-stress|pg18-not-null-constraint|pg18-temporal-constraints|pg18-partition-fk-not-valid|pg18-partition-ops)
      add_param "$option" "$(rand_int_range 0 50)"
      ;;
    pg18-vacuum-analyze-only|returning-old-new|pg18-merge|pg18-copy|pg18-explain|pg18-functions)
      add_param "$option" "$(rand_int_range 0 50)"
      ;;
    pg18-copy-reject-limit)
      add_param "$option" "$(rand_int_range 1 100)"
      ;;
    max-partitions)
      add_param "$option" "$(rand_int_range 1 128)"
      ;;
    *)
      add_param "$option" "$(generic_int_value "$option")"
      ;;
  esac
}

emit_bool_option() {
  local option="$1"
  local default_probability=5
  case "$option" in
    help|verbose|test-connection)
      return
      ;;
    prepare)
      if [[ "${PSTRESS_FORCE_PREPARE:-0}" == "1" ]]; then
        add_flag "$option"
      else
        maybe_add_flag "$option" 8 >/dev/null || true
      fi
      return
      ;;
    log-all-queries|log-query-duration)
      add_flag "$option"
      return
      ;;
    pquery|jlddl|only-cl-sql|only-cl-ddl)
      maybe_add_flag "$option" 1 >/dev/null || true
      return
      ;;
    no-select|no-insert|no-update|no-delete|no-ddl)
      maybe_add_flag "$option" 8 >/dev/null || true
      return
      ;;
    no-partition-tables|only-partition-tables|no-temp-tables|only-temp-tables|no-unlogged-tables|only-unlogged-tables|no-fk-tables)
      return
      ;;
    no-generated-columns|no-blob|no-auto-inc|no-desc-index|exact-initial-records|check-preload)
      maybe_add_flag "$option" 15 >/dev/null || true
      return
      ;;
    log-query-statistics|log-query-numbers|log-client-output|log-succeeded-queries|no-shuffle)
      maybe_add_flag "$option" 12 >/dev/null || true
      return
      ;;
    *)
      maybe_add_flag "$option" "$default_probability" >/dev/null || true
      ;;
  esac
}

emit_conflict_managed_flags() {
  local table_mode
  table_mode="$(rand_int_range 0 99)"
  if (( table_mode < 8 )) && has_option "only-partition-tables"; then
    add_flag "only-partition-tables"
  elif (( table_mode < 16 )) && has_option "no-partition-tables"; then
    add_flag "no-partition-tables"
  elif (( table_mode < 22 )) && has_option "only-temp-tables"; then
    add_flag "only-temp-tables"
  elif (( table_mode < 30 )) && has_option "no-temp-tables"; then
    add_flag "no-temp-tables"
  elif (( table_mode < 34 )) && has_option "only-unlogged-tables"; then
    add_flag "only-unlogged-tables"
  elif (( table_mode < 42 )) && has_option "no-unlogged-tables"; then
    add_flag "no-unlogged-tables"
  fi

  if has_option "no-fk-tables"; then
    maybe_add_flag "no-fk-tables" 8 >/dev/null || true
  fi
}

emit_string_options() {
  local option
  for option in "${STRING_OPTIONS[@]-}"; do
    case "$option" in
      address|database|user|password|socket|config-file|logdir|metadata-path)
        continue
        ;;
      help|verbose)
        continue
        ;;
      *)
        if chance_percent 20; then
          add_param "$option" "$(string_choice_for_option "$option")"
        fi
        ;;
    esac
  done
}

has_option() {
  local wanted="$1"
  local option
  for option in "${OPTION_NAMES[@]-}"; do
    if [[ "$option" == "$wanted" ]]; then
      return 0
    fi
  done
  return 1
}

emit_fixed_connection_params() {
  add_param "address" "$pg_host"
  add_param "port" "$pg_port"
  add_param "user" "$pg_user"
  add_param "database" "$pg_db"
  add_param "logdir" "$round_logdir"
  if [[ -n "$pg_password" ]] && has_option "password"; then
    add_param "password" "$pg_password"
  fi
}

emit_non_connection_options() {
  local option
  for option in "${INT_OPTIONS[@]-}"; do
    case "$option" in
      port)
        continue
        ;;
      seconds|seed)
        emit_curated_int "$option"
        ;;
      step)
        continue
        ;;
      *)
        emit_curated_int "$option"
        ;;
    esac
  done

  emit_conflict_managed_flags

  for option in "${BOOL_OPTIONS[@]-}"; do
    emit_bool_option "$option"
  done

  emit_string_options

  ensure_useful_mix
}

ensure_useful_mix() {
  local disable_count=0
  local line
  for line in "${PARAM_LINES[@]}"; do
    case "$line" in
      --no-select=true|--no-insert=true|--no-update=true|--no-delete=true|--no-ddl=true)
        disable_count=$(( disable_count + 1 ))
        ;;
    esac
  done

  if (( disable_count == 5 )); then
    add_param "select-single-row" "$(rand_int_range 1 1000)"
  fi
}

write_round_files() {
  local cmd_file="$1"
  local params_file="$2"
  local cmd_text
  cmd_text="$(shell_escape_array "${pstress_bin}" "${CMD_ARGS[@]}")"
  printf '%s\n' "$cmd_text" > "$cmd_file"
  printf '%s\n' "${PARAM_LINES[@]}" | LC_ALL=C sort > "$params_file"
}

run_server_check() {
  if command -v pg_isready >/dev/null 2>&1; then
    if [[ -n "$pg_password" ]]; then
      PGPASSWORD="$pg_password" pg_isready -h "$pg_host" -p "$pg_port" \
        -U "$pg_user" -d "$pg_db" >/dev/null
    else
      pg_isready -h "$pg_host" -p "$pg_port" -U "$pg_user" -d "$pg_db" \
        >/dev/null
    fi
    return
  fi

  if command -v psql >/dev/null 2>&1; then
    if [[ -n "$pg_password" ]]; then
      PGPASSWORD="$pg_password" psql -h "$pg_host" -p "$pg_port" \
        -U "$pg_user" -d "$pg_db" -c "select 1" >/dev/null
    else
      psql -h "$pg_host" -p "$pg_port" -U "$pg_user" -d "$pg_db" \
        -c "select 1" >/dev/null
    fi
    return
  fi

  die "pg_isready or psql is required for server health check"
}

has_prepare_requested() {
  local line
  for line in "${BASE_PARAM_LINES[@]-}"; do
    if [[ "$line" == "--prepare=true" ]]; then
      return 0
    fi
  done
  return 1
}

reset_phase_from_base() {
  CMD_ARGS=("${BASE_CMD_ARGS[@]}")
  PARAM_LINES=("${BASE_PARAM_LINES[@]}")
}

remove_flag_from_phase() {
  local name="$1"
  local flag="--${name}"
  local param_line="--${name}=true"
  local filtered=()
  local item
  for item in "${CMD_ARGS[@]-}"; do
    if [[ "$item" != "$flag" ]]; then
      filtered+=("$item")
    fi
  done
  CMD_ARGS=("${filtered[@]}")
  filtered=()
  for item in "${PARAM_LINES[@]-}"; do
    if [[ "$item" != "$param_line" ]]; then
      filtered+=("$item")
    fi
  done
  PARAM_LINES=("${filtered[@]}")
}

add_or_replace_param_in_phase() {
  local name="$1"
  local value="$2"
  local needle="--${name}="
  local replacement="--${name}=${value}"
  local i
  local found=0
  for (( i = 0; i < ${#CMD_ARGS[@]}; i++ )); do
    if [[ "${CMD_ARGS[$i]}" == ${needle}* ]]; then
      CMD_ARGS[$i]="$replacement"
      found=1
    fi
  done
  if (( ! found )); then
    CMD_ARGS+=("$replacement")
  fi
  found=0
  for (( i = 0; i < ${#PARAM_LINES[@]}; i++ )); do
    if [[ "${PARAM_LINES[$i]}" == ${needle}* ]]; then
      PARAM_LINES[$i]="$replacement"
      found=1
    fi
  done
  if (( ! found )); then
    PARAM_LINES+=("$replacement")
  fi
}

current_phase_step() {
  local item
  for item in "${CMD_ARGS[@]-}"; do
    if [[ "$item" == --step=* ]]; then
      printf '%s' "${item#*=}"
      return
    fi
  done
  printf '1'
}

find_first_failed_thread() {
  local step="$1"
  local files=()
  local file
  shopt -s nullglob
  files=("${round_logdir}"/*"_step_${step}_thread-"*.sql)
  shopt -u nullglob

  local best_key=""
  local best_thread="none"
  for file in "${files[@]-}"; do
    local match
    match="$(grep -n -m1 -E '(^|[[:space:]])F[[:space:]]|Error code|connection lost|failed, check logs' "$file" 2>/dev/null || true)"
    [[ -n "$match" ]] || continue

    local line_no="${match%%:*}"
    local line="${match#*:}"
    local offset
    offset="$(printf '%s\n' "$line" | sed -n 's/^[0-9T:-][0-9T:-]*[[:space:]]\([0-9][0-9]*\)=>.*/\1/p')"
    if [[ -z "$offset" ]]; then
      offset=999999999999
    fi

    local thread="unknown"
    local base
    base="$(basename "$file")"
    if [[ "$base" =~ (thread-[0-9]+)\.sql$ ]]; then
      thread="${BASH_REMATCH[1]}"
    fi

    local key
    key="$(printf '%012d:%012d:%s' "$offset" "$line_no" "$file")"
    if [[ -z "$best_key" || "$key" < "$best_key" ]]; then
      best_key="$key"
      best_thread="$thread"
    fi
  done

  printf '%s' "$best_thread"
}

execute_phase() {
  local round="$1"
  local phase="$2"
  local start_ts="$3"
  local cmd_file="$4"
  local params_file="$5"

  write_round_files "$cmd_file" "$params_file"

  set +e
  "${pstress_bin}" "${CMD_ARGS[@]}"
  local rc=$?
  set -e

  local server_status="ok"
  if ! run_server_check; then
    server_status="failed"
  fi

  local status="continue"
  if (( rc != 0 )) || [[ "$server_status" != "ok" ]]; then
    status="stop"
  fi

  local first_failed_thread
  first_failed_thread="$(find_first_failed_thread "$(current_phase_step)")"

  printf '%s round=%d phase=%s seed=%d exit_code=%d server_check=%s status=%s first_failed_thread=%s\n' \
    "$start_ts" "$round" "$phase" "$current_seed" "$rc" "$server_status" "$status" "$first_failed_thread" >> "$summary_log"

  [[ "$status" == "continue" ]]
}

parse_args() {
  pstress_bin=""
  pg_host=""
  pg_port=""
  pg_user=""
  pg_db=""
  pg_password=""
  logdir=""
  round_seconds=600
  max_rounds=0
  base_seed=""

  while [[ $# -gt 0 ]]; do
    case "$1" in
      --pstress-bin) pstress_bin="$(next_arg_value "$1" "${2:-}")"; shift 2 ;;
      --pstress-bin=*) pstress_bin="${1#*=}"; shift ;;
      --pg-host) pg_host="$(next_arg_value "$1" "${2:-}")"; shift 2 ;;
      --pg-host=*) pg_host="${1#*=}"; shift ;;
      --pg-port) pg_port="$(next_arg_value "$1" "${2:-}")"; shift 2 ;;
      --pg-port=*) pg_port="${1#*=}"; shift ;;
      --pg-user) pg_user="$(next_arg_value "$1" "${2:-}")"; shift 2 ;;
      --pg-user=*) pg_user="${1#*=}"; shift ;;
      --pg-db) pg_db="$(next_arg_value "$1" "${2:-}")"; shift 2 ;;
      --pg-db=*) pg_db="${1#*=}"; shift ;;
      --pg-password) pg_password="$(next_arg_value "$1" "${2:-}")"; shift 2 ;;
      --pg-password=*) pg_password="${1#*=}"; shift ;;
      --logdir) logdir="$(next_arg_value "$1" "${2:-}")"; shift 2 ;;
      --logdir=*) logdir="${1#*=}"; shift ;;
      --round-seconds) round_seconds="$(next_arg_value "$1" "${2:-}")"; shift 2 ;;
      --round-seconds=*) round_seconds="${1#*=}"; shift ;;
      --max-rounds) max_rounds="$(next_arg_value "$1" "${2:-}")"; shift 2 ;;
      --max-rounds=*) max_rounds="${1#*=}"; shift ;;
      --base-seed) base_seed="$(next_arg_value "$1" "${2:-}")"; shift 2 ;;
      --base-seed=*) base_seed="${1#*=}"; shift ;;
      --help|-h) usage; exit 0 ;;
      *) die "unknown argument: $1" ;;
    esac
  done

  [[ -n "$pstress_bin" ]] || die "--pstress-bin is required"
  [[ -x "$pstress_bin" ]] || die "--pstress-bin must be executable"
  [[ -n "$pg_host" ]] || die "--pg-host is required"
  [[ -n "$pg_port" ]] || die "--pg-port is required"
  [[ -n "$pg_user" ]] || die "--pg-user is required"
  [[ -n "$pg_db" ]] || die "--pg-db is required"
  [[ -n "$logdir" ]] || die "--logdir is required"
  [[ "$round_seconds" =~ ^[0-9]+$ ]] || die "--round-seconds must be numeric"
  [[ "$max_rounds" =~ ^[0-9]+$ ]] || die "--max-rounds must be numeric"
  if [[ -n "$base_seed" ]]; then
    [[ "$base_seed" =~ ^[0-9]+$ ]] || die "--base-seed must be numeric"
  else
    base_seed="$(date +%s)"
  fi
}

main() {
  require_cmd bash
  parse_args "$@"
  mkdir -p "$logdir"

  OPTION_NAMES=()
  OPTION_KINDS=()
  OPTION_DEFAULTS=()
  BOOL_OPTIONS=()
  INT_OPTIONS=()
  STRING_OPTIONS=()

  init_option_metadata

  summary_log="${logdir}/run-summary.log"
  if [[ ! -f "$summary_log" ]]; then
    printf 'timestamp round phase seed exit_code server_check status first_failed_thread\n' > "$summary_log"
  fi

  local round=1
  while :; do
    if (( max_rounds > 0 && round > max_rounds )); then
      break
    fi

    round_number="$round"
    current_seed=$(( base_seed + round ))
    RANDOM=$(( current_seed % 32767 ))

    local round_tag
    local host_tag
    local file_prefix
    round_tag="$(printf '%04d' "$round")"
    host_tag="$(safe_filename_component "$pg_host")"
    file_prefix="${host_tag}-round-${round_tag}"
    round_logdir="${logdir}"
    round_metadir="${logdir}/${file_prefix}.metadata"

    CMD_ARGS=()
    PARAM_LINES=()

    emit_fixed_connection_params
    emit_non_connection_options
    BASE_CMD_ARGS=("${CMD_ARGS[@]}")
    BASE_PARAM_LINES=("${PARAM_LINES[@]}")

    local start_ts
    start_ts="$(date '+%F %T')"

    if has_prepare_requested; then
      mkdir -p "$round_metadir"

      reset_phase_from_base
      add_or_replace_param_in_phase "metadata-path" "$round_metadir"
      if ! execute_phase "$round" "prepare" "$start_ts" \
        "${logdir}/${file_prefix}.prepare.cmd" \
        "${logdir}/${file_prefix}.prepare.params"; then
        break
      fi

      reset_phase_from_base
      remove_flag_from_phase "prepare"
      add_or_replace_param_in_phase "metadata-path" "$round_metadir"
      add_or_replace_param_in_phase "step" "2"
      if ! execute_phase "$round" "run" "$start_ts" \
        "${logdir}/${file_prefix}.run.cmd" \
        "${logdir}/${file_prefix}.run.params"; then
        break
      fi
    else
      reset_phase_from_base
      if ! execute_phase "$round" "run" "$start_ts" \
        "${logdir}/${file_prefix}.cmd" \
        "${logdir}/${file_prefix}.params"; then
        break
      fi
    fi

    round=$(( round + 1 ))
  done
}

main "$@"
