#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
BUILD_DIR="${PROJECT_ROOT}/build-wasix"
TOOLCHAIN_FILE="${PROJECT_ROOT}/wasix/wasix-toolchain.cmake"
OPENSSL_WASIX_DIR="${PROJECT_ROOT}/deps/openssl-wasix"
# Records the ABI of the shared deps/openssl-wasix libraries, so it stays in
# build-wasix even when building the embedded variant below.
OPENSSL_EXCEPTION_MODE_FILE="${PROJECT_ROOT}/build-wasix/.openssl-build-abi"
OPENSSL_BUILD_ABI="${WASIXCC_WASM_EXCEPTIONS:-exnref}-no-wide-arithmetic"
VALIDATE_WASM="${PROJECT_ROOT}/wasix/validate-imported-napi-wasm.py"
BUILD_TYPE="${CMAKE_BUILD_TYPE:-Release}"
STAGE_STATIC_DATA="${PROJECT_ROOT}/scripts/stage_wasix_static_data.py"

# Builtin JS and ICU data are read from the package filesystem (/node-lib and
# /usr/share/edge-icu, staged into ${BUILD_DIR}/node-lib and ${BUILD_DIR}/icu)
# instead of being embedded: wasm copies every data segment into each
# instance's linear memory. EDGE_WASIX_EMBED_STATIC_DATA=1 builds a standalone
# edge.wasm that embeds both, in its own build directory.
if [[ "${EDGE_WASIX_EMBED_STATIC_DATA:-0}" == "1" ]]; then
  EMBED_STATIC_DATA=ON
  BUILD_DIR="${PROJECT_ROOT}/build-wasix-embedded"
else
  EMBED_STATIC_DATA=OFF
fi

# Chrome and current Node support the standardized exnref exception-handling
# proposal, so build the guest and every static dependency with that ABI.
export WASIXCC_WASM_EXCEPTIONS="${WASIXCC_WASM_EXCEPTIONS:-exnref}"
export WASIXCC_RUN_WASM_OPT="${WASIXCC_RUN_WASM_OPT:-no}"

if ! command -v wasixcc >/dev/null 2>&1 && [[ -x "${HOME}/.wasixcc/bin/wasixcc" ]]; then
  export PATH="${HOME}/.wasixcc/bin:${PATH}"
fi

# Cross-building to WASIX must not inherit host include/link search paths from
# the shell environment, or CMake will persist them into linker flags.
for host_toolchain_var in \
  CPPFLAGS \
  LDFLAGS \
  LIBRARY_PATH \
  CPATH \
  C_INCLUDE_PATH \
  CPLUS_INCLUDE_PATH \
  OBJC_INCLUDE_PATH \
  SDKROOT
do
  unset "${host_toolchain_var}" || true
done
unset host_toolchain_var

optimize_wasm() {
  local input="$1"
  local output="$2"
  if command -v wasm-opt >/dev/null 2>&1; then
    wasm-opt -O2 --emit-exnref -o "${output}" "${input}"
    return
  fi
  echo "warning: wasm-opt not found in PATH; copying ${input} to ${output}" >&2
  cp "${input}" "${output}"
}

"${PROJECT_ROOT}/wasix/setup-wasix-deps.sh"
mkdir -p "${BUILD_DIR}" "$(dirname "${OPENSSL_EXCEPTION_MODE_FILE}")"

if [[ -f "${BUILD_DIR}/CMakeCache.txt" ]]; then
  rm -f "${BUILD_DIR}/CMakeCache.txt"
fi
if [[ -d "${BUILD_DIR}/CMakeFiles" ]]; then
  rm -rf "${BUILD_DIR}/CMakeFiles"
fi

openssl_wasix_ready() {
    [[ -f "${OPENSSL_WASIX_DIR}/libcrypto.a" ]] &&
    [[ -f "${OPENSSL_WASIX_DIR}/libssl.a" ]] &&
    [[ -f "${OPENSSL_WASIX_DIR}/include/openssl/ssl.h" ]] &&
    [[ -f "${OPENSSL_WASIX_DIR}/include/openssl/comp.h" ]] &&
    [[ -f "${OPENSSL_WASIX_DIR}/include/openssl/e_ostime.h" ]] &&
    [[ -f "${OPENSSL_WASIX_DIR}/include/openssl/lhash.h" ]] &&
    [[ -f "${OPENSSL_WASIX_DIR}/include/openssl/opensslv.h" ]] &&
    [[ "$(cat "${OPENSSL_EXCEPTION_MODE_FILE}" 2>/dev/null || true)" == "${OPENSSL_BUILD_ABI}" ]]
}

if ! openssl_wasix_ready; then
  echo "Building OpenSSL static libraries for WASIX..."
  (
    cd "${OPENSSL_WASIX_DIR}"
    make distclean >/dev/null 2>&1 || true
    CC=wasixcc \
    CXX=wasixcc++ \
    AR=wasixar \
    RANLIB=wasixranlib \
    NM=wasixnm \
    LD=wasixld \
    CFLAGS="--target=wasm32-wasix -matomics -mbulk-memory -mmutable-globals -mno-wide-arithmetic -pthread -mthread-model posix -ftls-model=local-exec -fno-trapping-math -D_WASI_EMULATED_MMAN -D_WASI_EMULATED_SIGNAL -D_WASI_EMULATED_PROCESS_CLOCKS -DUSE_TIMEGM -DOPENSSL_NO_SECURE_MEMORY -DOPENSSL_NO_DGRAM -DOPENSSL_THREADS -O2" \
    LDFLAGS="-Wl,--allow-undefined" \
    ./Configure linux-generic32 -static no-shared no-pic no-asm no-dso no-tests no-apps no-afalgeng -DUSE_TIMEGM -DOPENSSL_NO_SECURE_MEMORY -DOPENSSL_NO_DGRAM -DOPENSSL_THREADS
    make build_generated
    make -j4 libcrypto.a libssl.a
    wasixranlib libcrypto.a || true
    wasixranlib libssl.a || true
    printf '%s\n' "${OPENSSL_BUILD_ABI}" > "${OPENSSL_EXCEPTION_MODE_FILE}"
  )
fi

cmake \
  -S "${PROJECT_ROOT}" \
  -B "${BUILD_DIR}" \
  -U CMAKE_C_FLAGS \
  -U CMAKE_CXX_FLAGS \
  -U CMAKE_EXE_LINKER_FLAGS \
  -U CMAKE_SHARED_LINKER_FLAGS \
  -U CMAKE_MODULE_LINKER_FLAGS \
  -DCMAKE_BUILD_TYPE="${BUILD_TYPE}" \
  -DCMAKE_TOOLCHAIN_FILE="${TOOLCHAIN_FILE}" \
  -DEDGE_NAPI_PROVIDER=imports \
  -DEDGE_BUILD_CLI=ON \
  -DEDGE_EMBED_BUILTIN_SOURCES="${EMBED_STATIC_DATA}" \
  -DEDGE_EMBED_ICU_DATA="${EMBED_STATIC_DATA}" \
  -DBUILD_TESTING=OFF

# This artifact deliberately leaves N-API unresolved. The host supplies these
# imports and therefore owns the JavaScript engine used by EdgeJS.
grep -Fxq 'EDGE_NAPI_PROVIDER:STRING=imports' "${BUILD_DIR}/CMakeCache.txt" || {
  echo "error: WASIX build did not configure EDGE_NAPI_PROVIDER=imports" >&2
  exit 1
}
for static_data_option in EDGE_EMBED_BUILTIN_SOURCES EDGE_EMBED_ICU_DATA; do
  grep -Fxq "${static_data_option}:BOOL=${EMBED_STATIC_DATA}" "${BUILD_DIR}/CMakeCache.txt" || {
    echo "error: WASIX build did not configure ${static_data_option}=${EMBED_STATIC_DATA}" >&2
    exit 1
  }
done
unset static_data_option

cmake --build "${BUILD_DIR}" -j4

if [[ -f "${BUILD_DIR}/edge" ]]; then
  optimize_wasm "${BUILD_DIR}/edge" "${BUILD_DIR}/edge.wasm"
  cp "${BUILD_DIR}/edge.wasm" "${BUILD_DIR}/edgejs.wasm"
elif [[ -f "${BUILD_DIR}/ubi" ]]; then
  optimize_wasm "${BUILD_DIR}/ubi" "${BUILD_DIR}/edgejs.wasm"
  cp "${BUILD_DIR}/edgejs.wasm" "${BUILD_DIR}/edge.wasm"
else
  echo "error: expected ${BUILD_DIR}/edge or ${BUILD_DIR}/ubi after build" >&2
  exit 1
fi

# Stage /node-lib and /usr/share/edge-icu for the options that are OFF.
python3 "${STAGE_STATIC_DATA}" \
  --project-root "${PROJECT_ROOT}" \
  --build-dir "${BUILD_DIR}"

# With both static-data options OFF this also enforces the data-section and
# initial-memory size guard.
python3 "${VALIDATE_WASM}" \
  --cmake-cache "${BUILD_DIR}/CMakeCache.txt" \
  "${BUILD_DIR}/edgejs.wasm"

echo "Built WASIX targets at ${BUILD_DIR}/edge.wasm and ${BUILD_DIR}/edgejs.wasm"
