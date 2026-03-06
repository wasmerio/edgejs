#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
BUILD_DIR="${PROJECT_ROOT}/build-wasix"
TOOLCHAIN_FILE="${PROJECT_ROOT}/ubi/cmake/wasix-toolchain.cmake"
OPENSSL_WASIX_DIR="${PROJECT_ROOT}/deps/openssl-wasix"

export WASIXCC_WASM_EXCEPTIONS="${WASIXCC_WASM_EXCEPTIONS:-yes}"

"${SCRIPT_DIR}/setup-wasix-deps.sh"

if [[ ! -f "${OPENSSL_WASIX_DIR}/libcrypto.a" || ! -f "${OPENSSL_WASIX_DIR}/libssl.a" ]]; then
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
    CFLAGS="--target=wasm32-wasix -matomics -mbulk-memory -mmutable-globals -pthread -mthread-model posix -ftls-model=local-exec -fno-trapping-math -D_WASI_EMULATED_MMAN -D_WASI_EMULATED_SIGNAL -D_WASI_EMULATED_PROCESS_CLOCKS -DUSE_TIMEGM -DOPENSSL_NO_SECURE_MEMORY -DOPENSSL_NO_DGRAM -DOPENSSL_THREADS -O2" \
    LDFLAGS="-Wl,--allow-undefined" \
    ./Configure linux-generic32 -static no-shared no-pic no-asm no-tests no-apps no-afalgeng -DUSE_TIMEGM -DOPENSSL_NO_SECURE_MEMORY -DOPENSSL_NO_DGRAM -DOPENSSL_THREADS
    make build_generated
    make -j4 libcrypto.a libssl.a
    wasixranlib libcrypto.a || true
    wasixranlib libssl.a || true
  )
fi

cmake \
  -S "${PROJECT_ROOT}/ubi" \
  -B "${BUILD_DIR}" \
  -DCMAKE_TOOLCHAIN_FILE="${TOOLCHAIN_FILE}" \
  -DUBI_NAPI_PROVIDER=imports \
  -DUBI_BUILD_CLI=ON \
  -DBUILD_TESTING=OFF

cmake --build "${BUILD_DIR}" -j4

if [[ -f "${BUILD_DIR}/ubi" ]]; then
  wasm-opt --emit-exnref -o "${BUILD_DIR}/ubi.wasm" "${BUILD_DIR}/ubi"
fi

if [[ -f "${BUILD_DIR}/ubienv" ]]; then
  wasm-opt --emit-exnref -o "${BUILD_DIR}/ubienv.wasm" "${BUILD_DIR}/ubienv"
fi

echo "Built WASIX target at ${BUILD_DIR}/ubi.wasm"
