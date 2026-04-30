#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
EDGE_BINARY="${EDGE_BINARY:-${ROOT_DIR}/build-edge/edge}"
ARTIFACT_ROOT="${EDGE_PRODUCTION_READINESS_ARTIFACT_DIR:-${ROOT_DIR}/artifacts/production-readiness}"
RUN_ID="${EDGE_PRODUCTION_READINESS_RUN_ID:-$(date -u +%Y%m%dT%H%M%SZ)}"
OUT_DIR="${ARTIFACT_ROOT}/${RUN_ID}"
SUMMARY="${OUT_DIR}/summary.md"
COLD_START_RUNS="${EDGE_PRODUCTION_READINESS_COLD_START_RUNS:-5}"
CASE_TIMEOUT="${EDGE_PRODUCTION_READINESS_CASE_TIMEOUT:-30}"
SAFE_MODE="${EDGE_PRODUCTION_READINESS_SAFE:-0}"
WASMER_BIN="${WASMER_BIN:-wasmer}"
EDGE_WASMER_PACKAGE="${EDGE_WASMER_PACKAGE:-}"

mkdir -p "${OUT_DIR}"

case_index=0
passed=0
failed=0

write_header() {
  {
    printf '# Production Readiness Smoke Report\n\n'
    printf '| Field | Value |\n'
    printf '|---|---|\n'
    printf '| Run ID | `%s` |\n' "${RUN_ID}"
    printf '| Edge binary | `%s` |\n' "${EDGE_BINARY}"
    printf '| Safe mode requested | `%s` |\n' "${SAFE_MODE}"
    printf '| Cold-start runs | `%s` |\n' "${COLD_START_RUNS}"
    printf '| Case timeout | `%ss` |\n\n' "${CASE_TIMEOUT}"
    printf '## Results\n\n'
    printf '| Case | Status | Log |\n'
    printf '|---|---|---|\n'
  } > "${SUMMARY}"
}

append_result() {
  local name="$1"
  local status="$2"
  local log_name="$3"
  printf '| `%s` | %s | `%s` |\n' "${name}" "${status}" "${log_name}" >> "${SUMMARY}"
}

run_case() {
  local name="$1"
  shift
  case_index=$((case_index + 1))
  local safe_name
  safe_name="$(printf '%02d-%s.log' "${case_index}" "${name}" | tr ' /:' '---')"
  local log_path="${OUT_DIR}/${safe_name}"
  local pid
  local elapsed=0
  local status=0

  printf '[run] %s\n' "${name}"
  "$@" >"${log_path}" 2>&1 &
  pid=$!

  while kill -0 "${pid}" 2>/dev/null; do
    if [[ "${elapsed}" -ge "${CASE_TIMEOUT}" ]]; then
      printf 'case timed out after %ss\n' "${CASE_TIMEOUT}" >> "${log_path}"
      kill "${pid}" 2>/dev/null || true
      wait "${pid}" 2>/dev/null || true
      status=124
      break
    fi
    sleep 1
    elapsed=$((elapsed + 1))
  done

  if [[ "${status}" -eq 0 ]]; then
    wait "${pid}" || status=$?
  fi

  if [[ "${status}" -eq 0 ]]; then
    append_result "${name}" "PASS" "${safe_name}"
    passed=$((passed + 1))
    printf '[ok] %s\n' "${name}"
  else
    append_result "${name}" "FAIL" "${safe_name}"
    failed=$((failed + 1))
    printf '[fail] %s\n' "${name}" >&2
    sed -n '1,120p' "${log_path}" >&2
  fi
}

run_edge() {
  "${EDGE_BINARY}" "$@"
}

run_safe_edge() {
  local args=("${EDGE_BINARY}" "--safe" "--wasmer-bin" "${WASMER_BIN}")
  if [[ -n "${EDGE_WASMER_PACKAGE}" ]]; then
    args+=("--wasmer-package" "${EDGE_WASMER_PACKAGE}")
  fi
  args+=("$@")
  "${args[@]}"
}

run_repeated_cold_start() {
  local i
  for ((i = 1; i <= COLD_START_RUNS; i++)); do
    "${EDGE_BINARY}" -e ""
  done
  printf 'production-readiness:cold-start %s\n' "${COLD_START_RUNS}"
}

write_footer() {
  {
    printf '\n## Summary\n\n'
    printf '%s\n' "- Passed: ${passed}"
    printf '%s\n' "- Failed: ${failed}"
    printf '%s\n' "- Artifact directory: \`${OUT_DIR}\`"
  } >> "${SUMMARY}"
}

if [[ ! -x "${EDGE_BINARY}" ]]; then
  printf 'error: Edge binary is missing or not executable: %s\n' "${EDGE_BINARY}" >&2
  exit 1
fi

write_header

run_case "native-version" run_edge --version
run_case "native-eval" run_edge -e "console.log('production-readiness:eval')"
run_case "native-module-load" run_edge "${ROOT_DIR}/tests/production_readiness/fixtures/module-load.js"
run_case "native-http-loopback" run_edge "${ROOT_DIR}/tests/production_readiness/fixtures/http-loopback.js"
run_case "native-repeated-cold-start" run_repeated_cold_start

if [[ "${SAFE_MODE}" == "1" || "${SAFE_MODE}" == "true" ]]; then
  run_case "safe-eval" run_safe_edge -e "console.log('production-readiness:safe-eval')"
  run_case "safe-microtask" run_safe_edge -e "console.log('A'); queueMicrotask(() => console.log('B')); console.log('C');"
else
  printf '[skip] safe-mode smoke; set EDGE_PRODUCTION_READINESS_SAFE=1 to enable\n'
fi

write_footer

printf 'Production readiness smoke report: %s\n' "${SUMMARY}"

if [[ "${failed}" -ne 0 ]]; then
  exit 1
fi
