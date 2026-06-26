#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
BUILD_DIR="${PROJECT_ROOT}/build-quickjs-wasix"
TOOLCHAIN_FILE="${PROJECT_ROOT}/wasix/wasix-toolchain.cmake"
OPENSSL_WASIX_DIR="${PROJECT_ROOT}/deps/openssl-wasix"

export WASIXCC_WASM_EXCEPTIONS="${WASIXCC_WASM_EXCEPTIONS:-yes}"

if ! command -v wasixcc >/dev/null 2>&1 && [[ -x "${HOME}/.wasixcc/bin/wasixcc" ]]; then
  export PATH="${HOME}/.wasixcc/bin:${PATH}"
fi

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
    wasm-opt --emit-exnref -o "${output}" "${input}"
    return
  fi
  echo "warning: wasm-opt not found in PATH; copying ${input} to ${output}" >&2
  cp "${input}" "${output}"
}

check_no_napi_imports() {
  local wasm="$1"
  python3 - "${wasm}" <<'PY'
import re
import sys

path = sys.argv[1]
data = memoryview(open(path, "rb").read())
if data[:4] != b"\0asm":
    raise SystemExit(f"error: {path} is not a wasm module")

offset = 8

def read_u32():
    global offset
    result = 0
    shift = 0
    while True:
        byte = data[offset]
        offset += 1
        result |= (byte & 0x7f) << shift
        if byte < 0x80:
            return result
        shift += 7

def read_name():
    length = read_u32()
    global offset
    value = bytes(data[offset:offset + length]).decode("utf-8", "replace")
    offset += length
    return value

def skip_value_type():
    global offset
    offset += 1

def skip_limits():
    flags = read_u32()
    read_u32()
    if flags & 1:
        read_u32()

def skip_import_desc():
    global offset
    kind = data[offset]
    offset += 1
    if kind == 0:
        read_u32()
    elif kind == 1:
        skip_value_type()
        skip_limits()
    elif kind == 2:
        skip_limits()
    elif kind == 3:
        skip_value_type()
        offset += 1
    elif kind == 4:
        read_u32()
        read_u32()
    else:
        raise SystemExit(f"error: unsupported wasm import kind {kind}")

napi_imports = []
while offset < len(data):
    section_id = data[offset]
    offset += 1
    section_size = read_u32()
    section_end = offset + section_size
    if section_id == 2:
        for _ in range(read_u32()):
            module = read_name()
            name = read_name()
            skip_import_desc()
            if re.match(r"^(napi_|node_api_|unofficial_napi_)", name):
                napi_imports.append(f"{module}.{name}")
        break
    offset = section_end

if napi_imports:
    print("error: QuickJS wasm imports N-API symbols:", file=sys.stderr)
    for item in napi_imports:
        print(f"  {item}", file=sys.stderr)
    raise SystemExit(1)
PY
}

"${PROJECT_ROOT}/wasix/setup-wasix-deps.sh"

if [[ "${WASIX_FORCE_RECONFIGURE:-0}" == "1" ]]; then
  if [[ -f "${BUILD_DIR}/CMakeCache.txt" ]]; then
    rm -f "${BUILD_DIR}/CMakeCache.txt"
  fi
  if [[ -d "${BUILD_DIR}/CMakeFiles" ]]; then
    rm -rf "${BUILD_DIR}/CMakeFiles"
  fi
fi

openssl_wasix_ready() {
    [[ -f "${OPENSSL_WASIX_DIR}/libcrypto.a" ]] &&
    [[ -f "${OPENSSL_WASIX_DIR}/libssl.a" ]] &&
    [[ -f "${OPENSSL_WASIX_DIR}/include/openssl/ssl.h" ]] &&
    [[ -f "${OPENSSL_WASIX_DIR}/include/openssl/comp.h" ]] &&
    [[ -f "${OPENSSL_WASIX_DIR}/include/openssl/e_ostime.h" ]] &&
    [[ -f "${OPENSSL_WASIX_DIR}/include/openssl/lhash.h" ]] &&
    [[ -f "${OPENSSL_WASIX_DIR}/include/openssl/opensslv.h" ]]
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
    CFLAGS="--target=wasm32-wasix -matomics -mbulk-memory -mmutable-globals -pthread -mthread-model posix -ftls-model=local-exec -fno-trapping-math -D_WASI_EMULATED_MMAN -D_WASI_EMULATED_SIGNAL -D_WASI_EMULATED_PROCESS_CLOCKS -DUSE_TIMEGM -DOPENSSL_NO_SECURE_MEMORY -DOPENSSL_NO_DGRAM -DOPENSSL_THREADS -O2" \
    LDFLAGS="-Wl,--allow-undefined" \
    ./Configure linux-generic32 -static no-shared no-pic no-asm no-dso no-tests no-apps no-afalgeng -DUSE_TIMEGM -DOPENSSL_NO_SECURE_MEMORY -DOPENSSL_NO_DGRAM -DOPENSSL_THREADS
    make build_generated
    make -j4 libcrypto.a libssl.a
    wasixranlib libcrypto.a || true
    wasixranlib libssl.a || true
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
  -DCMAKE_TOOLCHAIN_FILE="${TOOLCHAIN_FILE}" \
  -DEDGE_NAPI_PROVIDER=quickjs \
  -DEDGE_BUILD_CLI=ON \
  -DBUILD_TESTING=OFF

cmake --build "${BUILD_DIR}" -j4

if [[ -f "${BUILD_DIR}/edge" ]]; then
  optimize_wasm "${BUILD_DIR}/edge" "${BUILD_DIR}/edge.wasm"
  cp "${BUILD_DIR}/edge.wasm" "${BUILD_DIR}/edgejs.wasm"
elif [[ -f "${BUILD_DIR}/edge.wasm" ]]; then
  optimize_wasm "${BUILD_DIR}/edge.wasm" "${BUILD_DIR}/edgejs.wasm"
else
  echo "error: expected ${BUILD_DIR}/edge.wasm or ${BUILD_DIR}/edge after build" >&2
  exit 1
fi

check_no_napi_imports "${BUILD_DIR}/edgejs.wasm"

echo "Built QuickJS WASIX targets at ${BUILD_DIR}/edge.wasm and ${BUILD_DIR}/edgejs.wasm"
