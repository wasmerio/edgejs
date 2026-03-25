#!/usr/bin/env bash
set -euo pipefail

BENCHMARK="${1:-console-log}"
EDGE_BIN="${EDGE_BIN:-./build-edge/edge}"
NODE_BIN="${NODE_BIN:-node}"
WARMUP="${WARMUP:-3}"
RUNS="${RUNS:-20}"
RESULTS_DIR="${RESULTS_DIR:-benchmarks/results}"

mkdir -p "$RESULTS_DIR"

case "$BENCHMARK" in
  empty-startup)
    EDGE_CMD="$EDGE_BIN benchmarks/workloads/empty-startup.js"
    NODE_CMD="$NODE_BIN benchmarks/workloads/empty-startup.js"
    ;;
  console-log)
    EDGE_CMD="$EDGE_BIN benchmarks/workloads/console-log.js"
    NODE_CMD="$NODE_BIN benchmarks/workloads/console-log.js"
    ;;
  json-parse-stringify)
    EDGE_CMD="$EDGE_BIN benchmarks/workloads/json-parse-stringify.js"
    NODE_CMD="$NODE_BIN benchmarks/workloads/json-parse-stringify.js"
    ;;
  promise-microtask-chain)
    EDGE_CMD="$EDGE_BIN benchmarks/workloads/promise-microtask-chain.js"
    NODE_CMD="$NODE_BIN benchmarks/workloads/promise-microtask-chain.js"
    ;;
  *)
    echo "Unknown benchmark: $BENCHMARK"
    echo "Available benchmarks: empty-startup, console-log, json-parse-stringify, promise-microtask-chain"
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
  --command-name node "$NODE_CMD"

echo
echo "Exported:"
echo "  $JSON_OUT"
echo "  $CSV_OUT"
echo "  $MD_OUT"
