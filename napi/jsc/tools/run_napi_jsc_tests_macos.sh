#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
project_root="$(cd "${script_dir}/../../.." && pwd)"
default_tag="$(bash "${script_dir}/bun_webkit_release_tag.sh")"

arch="$(uname -m)"
case "${arch}" in
  arm64) asset_arch="arm64" ;;
  x86_64) asset_arch="amd64" ;;
  *) asset_arch="${arch}" ;;
esac

default_webkit_root="/Users/syrusakbary/Development/WebKit"
if [[ "${HOME:-}" != "/Users/syrusakbary" ]]; then
  default_webkit_root="${HOME}/Development/WebKit"
fi

default_bun_root="${project_root}/.ci/jsc/${default_tag}/macos-${asset_arch}"
if [[ ! -d "${default_bun_root}" ]]; then
  default_bun_root="/Users/syrusakbary/Development/bun-webkit/${default_tag}"
  if [[ "${HOME:-}" != "/Users/syrusakbary" ]]; then
    default_bun_root="${HOME}/Development/bun-webkit/${default_tag}"
  fi
fi

webkit_root="${WEBKIT_ROOT:-$default_webkit_root}"
webkit_configuration="${WEBKIT_CONFIGURATION:-Release}"
bun_root="${BUN_WEBKIT_ROOT:-$default_bun_root}"
build_dir="${BUILD_DIR:-${project_root}/build-napi-jsc}"
stock_host_build_dir="${STOCK_HOST_BUILD_DIR:-${project_root}/build-jsc-stock-host}"
jobs="${JOBS:-$(sysctl -n hw.ncpu)}"
probe_output="${build_dir}/jsc-runtime-probe.txt"
report_output="${build_dir}/jsc-runtime-report.txt"
smoke_regex='napi_jsc_test_(29_bigint|31_dataview|32_sharedarraybuffer|35_promise|39_cannot_run_js|65_unofficial_contextify|66_unofficial_unsupported)'

runtime_kind=""
runtime_root=""
runtime_commit="n/a"
runtime_product_dir=""
jsc_headers=""
jsc_library=""
jsc_extra_libs=""

log() {
  printf '[run_napi_jsc_tests_macos] %s\n' "$*"
}

fail() {
  log "$*"
  exit 1
}

prepend_env_path() {
  local var_name="$1"
  local path_value="$2"
  local current_value="${!var_name-}"
  if [[ -z "${current_value}" ]]; then
    export "${var_name}=${path_value}"
  else
    export "${var_name}=${path_value}:${current_value}"
  fi
}

if [[ "$(uname -s)" != "Darwin" ]]; then
  fail "This script only supports macOS."
fi

if [[ -d "${bun_root}/include" && -f "${bun_root}/lib/libJavaScriptCore.a" ]]; then
  runtime_kind="bun-webkit-static"
  runtime_root="${bun_root}"
  runtime_product_dir="${bun_root}"
  jsc_headers="${bun_root}/include"
  jsc_library="${bun_root}/lib/libJavaScriptCore.a"
  jsc_extra_libs="${bun_root}/lib/libWTF.a;${bun_root}/lib/libbmalloc.a;icucore"
  if [[ -f "${bun_root}/package.json" ]]; then
    runtime_commit="$(python3 - <<'PY' "${bun_root}/package.json"
import json
import sys
with open(sys.argv[1], 'r', encoding='utf-8') as handle:
    package = json.load(handle)
version = package.get('version', '')
print(version.split('-', 1)[1] if '-' in version else version or 'unknown')
PY
)"
  fi
else
  runtime_kind="webkit-framework"
  runtime_root="${webkit_root}"
  runtime_product_dir="${webkit_root}/WebKitBuild/${webkit_configuration}"
  jsc_headers="${runtime_product_dir}/JavaScriptCore.framework/Headers"
  jsc_library="${runtime_product_dir}/JavaScriptCore.framework/Versions/A/JavaScriptCore"
  [[ -d "${jsc_headers}" ]] || fail "Missing JavaScriptCore headers at ${jsc_headers}. Run napi/jsc/tools/build_webkit_macos.sh first or set BUN_WEBKIT_ROOT."
  [[ -f "${jsc_library}" ]] || fail "Missing JavaScriptCore library at ${jsc_library}. Run napi/jsc/tools/build_webkit_macos.sh first or set BUN_WEBKIT_ROOT."
  runtime_commit="$(git -C "${webkit_root}" rev-parse HEAD)"
fi

[[ -d "${jsc_headers}" ]] || fail "Missing JavaScriptCore headers at ${jsc_headers}"
[[ -f "${jsc_library}" ]] || fail "Missing JavaScriptCore library at ${jsc_library}"

generator_args=()
if command -v ninja >/dev/null 2>&1; then
  generator_args=(-G Ninja)
fi

mkdir -p "${build_dir}"

build_env=()
if [[ "${runtime_kind}" == "bun-webkit-static" ]]; then
  build_env+=(BUN_WEBKIT_ROOT="${runtime_root}")
else
  build_env+=(
    NAPI_JSC_INCLUDE_DIR="${jsc_headers}"
    NAPI_JSC_LIBRARY="${jsc_library}"
    NAPI_JSC_EXTRA_LIBS="${jsc_extra_libs}"
  )
fi

log "Building JSC provider tests in ${build_dir} via make build-napi-jsc"
env "${build_env[@]}" \
  make -C "${project_root}" build-napi-jsc \
    BUILD_DIR="${build_dir}" \
    JOBS="${jobs}" \
    CMAKE_BUILD_TYPE=Release \
    EXTRA_CMAKE_ARGS="${generator_args[*]}"

probe_bin="${build_dir}/napi-jsc/tests/napi_jsc_runtime_probe"
guard_bin="${build_dir}/napi-jsc/tests/napi_jsc_test_67_jsc_config_guard"
sample_test_bin="${build_dir}/napi-jsc/tests/napi_jsc_test_32_sharedarraybuffer"

[[ -x "${probe_bin}" ]] || fail "Runtime probe was not built at ${probe_bin}"
[[ -x "${guard_bin}" ]] || fail "Negative guard test was not built at ${guard_bin}"

if [[ "${RUN_STOCK_HOST_NEGATIVE_GUARD:-1}" == "1" ]]; then
  mkdir -p "${stock_host_build_dir}"
  log "Configuring stock-host negative guard build in ${stock_host_build_dir}"
  cmake -S "${project_root}/napi/jsc" -B "${stock_host_build_dir}" \
    "${generator_args[@]}" \
    -DCMAKE_BUILD_TYPE=Release \
    -DNAPI_JSC_BUILD_TESTS=ON

  log "Building stock-host negative guard target"
  cmake --build "${stock_host_build_dir}" --parallel "${jobs}" --target napi_jsc_test_67_jsc_config_guard

  log "Running stock-host negative guard"
  env -u DYLD_FRAMEWORK_PATH -u DYLD_LIBRARY_PATH -u __XPC_DYLD_FRAMEWORK_PATH -u __XPC_DYLD_LIBRARY_PATH \
    "${stock_host_build_dir}/tests/napi_jsc_test_67_jsc_config_guard" \
    --gtest_filter=Test67JscConfigGuard.RejectsSablessHostConfiguration
fi

if [[ "${runtime_kind}" == "webkit-framework" ]]; then
  prepend_env_path DYLD_FRAMEWORK_PATH "${runtime_product_dir}"
  prepend_env_path DYLD_LIBRARY_PATH "${runtime_product_dir}"
  prepend_env_path __XPC_DYLD_FRAMEWORK_PATH "${runtime_product_dir}"
  prepend_env_path __XPC_DYLD_LIBRARY_PATH "${runtime_product_dir}"
  export WEBKIT_UNSET_DYLD_FRAMEWORK_PATH=YES
fi

log "Running runtime probe"
"${probe_bin}" | tee "${probe_output}"

if [[ -x "${sample_test_bin}" ]]; then
  log "Inspecting test linkage"
  otool -L "${sample_test_bin}" | tee "${build_dir}/jsc-runtime-otool.txt"
fi

log "Running JSC smoke subset"
ctest --test-dir "${build_dir}" --output-on-failure -j "${jobs}" -R "${smoke_regex}"

log "Running full JSC ctest suite"
ctest --test-dir "${build_dir}" --output-on-failure -j "${jobs}" -R '^napi_jsc\.'

{
  printf 'Runtime kind: %s\n' "${runtime_kind}"
  printf 'Runtime root: %s\n' "${runtime_root}"
  printf 'Runtime commit: %s\n' "${runtime_commit}"
  printf 'Runtime product dir: %s\n' "${runtime_product_dir}"
  printf 'NAPI_JSC_INCLUDE_DIR: %s\n' "${jsc_headers}"
  printf 'NAPI_JSC_LIBRARY: %s\n' "${jsc_library}"
  printf 'NAPI_JSC_EXTRA_LIBS: %s\n' "${jsc_extra_libs}"
  printf 'Probe output: %s\n' "${probe_output}"
  printf 'Build directory: %s\n' "${build_dir}"
  printf 'Smoke regex: %s\n' "${smoke_regex}"
  printf 'ctest command: ctest --test-dir %s --output-on-failure -j %s -R ^napi_jsc\\.\n' "${build_dir}" "${jobs}"
} > "${report_output}"

log "Wrote runtime report to ${report_output}"
log "Validated ${runtime_kind} runtime ${runtime_commit}"
