#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 1 ]]; then
  echo "usage: $0 <test-name>" >&2
  echo "test-name maps to tests/programs/<test-name>.c" >&2
  exit 1
fi

TEST_NAME="$1"

# ROOT_DIR = napi/wasmer (the crate root)
ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")"/../ && pwd)"
# PROJECT_ROOT = top-level (node-napi)
PROJECT_ROOT="$ROOT_DIR/../.."
OUT_DIR="$ROOT_DIR/target/native"
OUT_FILE="$OUT_DIR/${TEST_NAME}"
TEST_SRC="$ROOT_DIR/tests/programs/${TEST_NAME}.c"
NAPI_INCLUDE_DIR="$ROOT_DIR/../include"
TEST_INCLUDE_DIR="$ROOT_DIR/tests/programs"
NATIVE_INIT_SRC="$ROOT_DIR/tests/napi_native_init.cc"

# napi/v8 paths
NAPI_V8_DIR="$PROJECT_ROOT/napi/v8"
NAPI_V8_INCLUDE="$PROJECT_ROOT/napi/include"
NAPI_V8_SRC="$NAPI_V8_DIR/src"
EDGE_SRC="$PROJECT_ROOT/src"
LIBUV_INCLUDE="$PROJECT_ROOT/deps/uv/include"

# V8 paths (auto-detect Homebrew)
V8_INCLUDE_DIR="${V8_INCLUDE_DIR:-/opt/homebrew/Cellar/v8/14.5.201.9/include}"
V8_LIB_DIR="${V8_LIB_DIR:-/opt/homebrew/Cellar/v8/14.5.201.9/lib}"
V8_DEFINES="${V8_DEFINES:-${NAPI_V8_DEFINES:-V8_COMPRESS_POINTERS}}"

if [[ ! -f "$TEST_SRC" ]]; then
  echo "test not found: $TEST_SRC" >&2
  exit 1
fi

mkdir -p "$OUT_DIR"

if [[ -f "$OUT_FILE" && "$OUT_FILE" -nt "$TEST_SRC" ]]; then
  echo "Up-to-date: $OUT_FILE"
  exit 0
fi

# Step 1: Compile the C test
clang -c -std=c11 -O2 \
  -DNAPI_EXTERN= \
  -DNAPI_VERSION=8 \
  -I"$NAPI_INCLUDE_DIR" \
  -I"$TEST_INCLUDE_DIR" \
  "$TEST_SRC" \
  -o "$OUT_DIR/${TEST_NAME}.o"

# Step 2: Compile native init + napi/v8 and link everything
clang++ -std=c++20 -O2 \
  -DNAPI_EXTERN= \
  -DNAPI_VERSION=8 \
  $(echo "$V8_DEFINES" | tr ';,' '\n' | sed '/^[[:space:]]*$/d; s/^[[:space:]]*/-D/; s/[[:space:]]*$//' | tr '\n' ' ') \
  -I"$NAPI_INCLUDE_DIR" \
  -I"$NAPI_V8_INCLUDE" \
  -I"$NAPI_V8_SRC" \
  -I"$EDGE_SRC" \
  -I"$LIBUV_INCLUDE" \
  -I"$V8_INCLUDE_DIR" \
  "$OUT_DIR/${TEST_NAME}.o" \
  "$NATIVE_INIT_SRC" \
  "$EDGE_SRC/edge_environment.cc" \
  "$NAPI_V8_SRC/js_native_api_v8.cc" \
  "$NAPI_V8_SRC/unofficial_napi.cc" \
  "$NAPI_V8_SRC/unofficial_napi_error_utils.cc" \
  "$NAPI_V8_SRC/unofficial_napi_contextify.cc" \
  "$NAPI_V8_SRC/edge_v8_platform.cc" \
  -L"$V8_LIB_DIR" \
  -lv8 \
  -lv8_libplatform \
  -lv8_libbase \
  -Wl,-rpath,"$V8_LIB_DIR" \
  -o "$OUT_FILE"

# Clean up intermediate object file
rm -f "$OUT_DIR/${TEST_NAME}.o"

echo "Built: $OUT_FILE"
