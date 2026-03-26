#!/usr/bin/env bash
set -euo pipefail

BENCHMARK="${1:-console-log}"
EDGE_BIN="${EDGE_BIN:-./build-edge/edge}"
NODE_BIN="${NODE_BIN:-node}"
BUN_BIN="${BUN_BIN:-bun}"
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

case "$BENCHMARK" in
  empty-startup)
    EDGE_CMD="$EDGE_BIN benchmarks/workloads/empty-startup.js"
    NODE_CMD="$NODE_BIN benchmarks/workloads/empty-startup.js"
    BUN_CMD="$BUN_BIN benchmarks/workloads/empty-startup.js"
    ;;
  console-log)
    EDGE_CMD="$EDGE_BIN benchmarks/workloads/console-log.js"
    NODE_CMD="$NODE_BIN benchmarks/workloads/console-log.js"
    BUN_CMD="$BUN_BIN benchmarks/workloads/console-log.js"
    ;;
  json-parse-stringify)
    EDGE_CMD="$EDGE_BIN benchmarks/workloads/json-parse-stringify.js"
    NODE_CMD="$NODE_BIN benchmarks/workloads/json-parse-stringify.js"
    BUN_CMD="$BUN_BIN benchmarks/workloads/json-parse-stringify.js"
    ;;
  promise-microtask-chain)
    EDGE_CMD="$EDGE_BIN benchmarks/workloads/promise-microtask-chain.js"
    NODE_CMD="$NODE_BIN benchmarks/workloads/promise-microtask-chain.js"
    BUN_CMD="$BUN_BIN benchmarks/workloads/promise-microtask-chain.js"
    ;;
  zlib-deflate-sync)
    EDGE_CMD="$EDGE_BIN benchmarks/workloads/zlib-deflate-sync.js"
    NODE_CMD="$NODE_BIN benchmarks/workloads/zlib-deflate-sync.js"
    BUN_CMD="$BUN_BIN benchmarks/workloads/zlib-deflate-sync.js"
    ;;
  string-compare-split)
    EDGE_CMD="$EDGE_BIN benchmarks/workloads/string-compare-split.js"
    NODE_CMD="$NODE_BIN benchmarks/workloads/string-compare-split.js"
    BUN_CMD="$BUN_BIN benchmarks/workloads/string-compare-split.js"
    ;;
  *)
    echo "Unknown benchmark: $BENCHMARK"
    echo "Available benchmarks: empty-startup, console-log, json-parse-stringify, promise-microtask-chain, zlib-deflate-sync, string-compare-split"
    exit 1
    ;;
esac

JSON_OUT="$RESULTS_DIR/${BENCHMARK}.json"
CSV_OUT="$RESULTS_DIR/${BENCHMARK}.csv"
MD_OUT="$RESULTS_DIR/${BENCHMARK}.md"

hyperfine \
  --warmup "$WARMUP" \
  --runs "$RUNS" \
  --export-json "$JSON_OUT" \
  --export-csv "$CSV_OUT" \
  --export-markdown "$MD_OUT" \
  --command-name edge "$EDGE_CMD" \
  --command-name node "$NODE_CMD" \
  --command-name bun "$BUN_CMD"

echo
echo "Exported:"
echo "  $JSON_OUT"
echo "  $CSV_OUT"
echo "  $MD_OUT"