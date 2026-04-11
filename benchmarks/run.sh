#!/usr/bin/env bash
set -euo pipefail

BENCHMARK="${1:-console-log}"
EDGE_BIN="${EDGE_BIN:-./build-edge/edge}"
NODE_BIN="${NODE_BIN:-node}"
BUN_BIN="${BUN_BIN:-bun}"
DENO_BIN="${DENO_BIN:-deno}"
WARMUP="${WARMUP:-3}"
RUNS="${RUNS:-20}"
RESULTS_DIR="${RESULTS_DIR:-benchmarks/results}"

mkdir -p "$RESULTS_DIR"

command -v hyperfine >/dev/null 2>&1 || {
  echo "Missing dependency: hyperfine"
  echo "Install it first, then re-run this script."
  exit 1
}

[[ -x "$EDGE_BIN" ]] || {
  echo "Missing edge binary: $EDGE_BIN"
  echo "Build Edge first with: make build"
  exit 1
}

command -v "$NODE_BIN" >/dev/null 2>&1 || {
  echo "Missing node binary: $NODE_BIN"
  exit 1
}

command -v "$BUN_BIN" >/dev/null 2>&1 || {
  echo "Missing bun binary: $BUN_BIN"
  echo "Install Bun first, then re-run this script."
  exit 1
}

command -v "$DENO_BIN" >/dev/null 2>&1 || {
  echo "Missing deno binary: $DENO_BIN"
  echo "Install Deno first, then re-run this script."
  exit 1
}

case "$BENCHMARK" in
  empty-startup)
    EDGE_CMD="$EDGE_BIN benchmarks/workloads/empty-startup.js"
    NODE_CMD="$NODE_BIN benchmarks/workloads/empty-startup.js"
    BUN_CMD="$BUN_BIN benchmarks/workloads/empty-startup.js"
    DENO_CMD="$DENO_BIN run benchmarks/workloads/empty-startup.js"
    ;;
  console-log)
    EDGE_CMD="$EDGE_BIN benchmarks/workloads/console-log.js"
    NODE_CMD="$NODE_BIN benchmarks/workloads/console-log.js"
    BUN_CMD="$BUN_BIN benchmarks/workloads/console-log.js"
    DENO_CMD="$DENO_BIN run benchmarks/workloads/console-log.js"
    ;;
  json-parse-stringify)
    EDGE_CMD="$EDGE_BIN benchmarks/workloads/json-parse-stringify.js"
    NODE_CMD="$NODE_BIN benchmarks/workloads/json-parse-stringify.js"
    BUN_CMD="$BUN_BIN benchmarks/workloads/json-parse-stringify.js"
    DENO_CMD="$DENO_BIN run benchmarks/workloads/json-parse-stringify.js"
    ;;
  promise-microtask-chain)
    EDGE_CMD="$EDGE_BIN benchmarks/workloads/promise-microtask-chain.js"
    NODE_CMD="$NODE_BIN benchmarks/workloads/promise-microtask-chain.js"
    BUN_CMD="$BUN_BIN benchmarks/workloads/promise-microtask-chain.js"
    DENO_CMD="$DENO_BIN run benchmarks/workloads/promise-microtask-chain.js"
    ;;
  zlib-deflate-sync)
    EDGE_CMD="$EDGE_BIN benchmarks/workloads/zlib-deflate-sync.js"
    NODE_CMD="$NODE_BIN benchmarks/workloads/zlib-deflate-sync.js"
    BUN_CMD="$BUN_BIN benchmarks/workloads/zlib-deflate-sync.js"
    DENO_CMD="$DENO_BIN run benchmarks/workloads/zlib-deflate-sync.js"
    ;;
  string-compare-split)
    EDGE_CMD="$EDGE_BIN benchmarks/workloads/string-compare-split.js"
    NODE_CMD="$NODE_BIN benchmarks/workloads/string-compare-split.js"
    BUN_CMD="$BUN_BIN benchmarks/workloads/string-compare-split.js"
    DENO_CMD="$DENO_BIN run benchmarks/workloads/string-compare-split.js"
    ;;
  cli-eval-empty)
    EDGE_CMD="$EDGE_BIN -e \"\""
    NODE_CMD="$NODE_BIN -e \"\""
    BUN_CMD="$BUN_BIN -e \"\""
    DENO_CMD="$DENO_BIN eval \"\""
    ;;
  cli-print-literal)
    EDGE_CMD="$EDGE_BIN -p \"1\""
    NODE_CMD="$NODE_BIN -p \"1\""
    BUN_CMD="$BUN_BIN -p \"1\""
    DENO_CMD="$DENO_BIN eval \"console.log(1)\""
    ;;
  cli-print-process-version)
    EDGE_CMD="$EDGE_BIN -p \"process.version\""
    NODE_CMD="$NODE_BIN -p \"process.version\""
    BUN_CMD="$BUN_BIN -p \"process.version\""
    DENO_CMD="$DENO_BIN eval \"console.log(process.version)\""
    ;;
  http-loopback)
    EDGE_CMD="$EDGE_BIN benchmarks/workloads/http-loopback.js"
    NODE_CMD="$NODE_BIN benchmarks/workloads/http-loopback.js"
    BUN_CMD="$BUN_BIN benchmarks/workloads/http-loopback.js"
    DENO_CMD="$DENO_BIN run --allow-net=127.0.0.1 benchmarks/workloads/http-loopback.js"
    ;;
  dependency-heavy-package)
    EDGE_CMD="$EDGE_BIN benchmarks/workloads/dependency-heavy-package.js"
    NODE_CMD="$NODE_BIN benchmarks/workloads/dependency-heavy-package.js"
    BUN_CMD="$BUN_BIN benchmarks/workloads/dependency-heavy-package.js"
    DENO_CMD="$DENO_BIN run benchmarks/workloads/dependency-heavy-package.js"
    ;;
  *)
    echo "Unknown benchmark: $BENCHMARK"
    echo "Available benchmarks: empty-startup, console-log, json-parse-stringify, promise-microtask-chain, zlib-deflate-sync, string-compare-split, cli-eval-empty, cli-print-literal, cli-print-process-version, http-loopback, dependency-heavy-package"
    exit 1
    ;;
esac

if [[ -n "${DENO_CMD:-}" ]]; then
  command -v "$DENO_BIN" >/dev/null 2>&1 || {
    echo "Missing deno binary: $DENO_BIN"
    echo "Install Deno first, then re-run this script."
    exit 1
  }
fi

echo "EDGE_CMD: $EDGE_CMD"

JSON_OUT="$RESULTS_DIR/${BENCHMARK}.json"
CSV_OUT="$RESULTS_DIR/${BENCHMARK}.csv"
MD_OUT="$RESULTS_DIR/${BENCHMARK}.md"
PROFILE_JSON_OUT="$RESULTS_DIR/${BENCHMARK}.edge-profile.json"
PROFILE_MD_OUT="$RESULTS_DIR/${BENCHMARK}.edge-profile.md"

COMMAND_ARGS=(
  --command-name edge "$EDGE_CMD"
  --command-name node "$NODE_CMD"
  --command-name bun "$BUN_CMD"
  --command-name deno "$DENO_CMD"
)


hyperfine \
  --warmup "$WARMUP" \
  --runs "$RUNS" \
  --export-json "$JSON_OUT" \
  --export-csv "$CSV_OUT" \
  --export-markdown "$MD_OUT" \
  "${COMMAND_ARGS[@]}"

"$NODE_BIN" benchmarks/capture-edge-startup-profile.mjs \
  "$PROFILE_JSON_OUT" \
  "$PROFILE_MD_OUT" \
  "$EDGE_CMD"

echo
echo "Exported:"
echo "  $JSON_OUT"
echo "  $CSV_OUT"
echo "  $MD_OUT"
echo "  $PROFILE_JSON_OUT"
echo "  $PROFILE_MD_OUT"
