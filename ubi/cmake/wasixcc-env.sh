#!/usr/bin/env sh

export WASIXCC_WASM_EXCEPTIONS=yes

WASIXCC_DRIVER="${1:-wasixcc}"
WASIXCC_DEFAULT_SYSROOT="$("${WASIXCC_DRIVER}" --print-sysroot 2>/dev/null || true)"
WASIXCC_SYSROOT_PREFIX=""
if [ -n "${WASIXCC_DEFAULT_SYSROOT}" ]; then
  WASIXCC_SYSROOT_PREFIX="$(dirname "${WASIXCC_DEFAULT_SYSROOT}")"
fi
WASIXCC_EXNREF_SYSROOT="${WASIXCC_SYSROOT_PREFIX}/sysroot-exnref-eh"

# Keep CI/default installations working by only switching sysroot
# when the exnref variant actually exists.
if [ -z "${WASIXCC_SYSROOT:-}" ] && [ -n "${WASIXCC_SYSROOT_PREFIX}" ] && [ -d "${WASIXCC_EXNREF_SYSROOT}" ]; then
  export WASIXCC_SYSROOT="${WASIXCC_EXNREF_SYSROOT}"
fi
