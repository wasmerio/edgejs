#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
default_edgejs_root="$(cd "${script_dir}/.." && pwd)"

edgejs_root="${EDGEJS_ROOT:-${default_edgejs_root}}"
runner_kind="${WASIX_RUNNER_KIND:-wasmer}"
runner_bin="${WASIX_RUNNER_BIN:-${WASMER_BIN:-wasmer}}"
package_dir="${WASIX_EDGEJS_PACKAGE_DIR:-${edgejs_root}/quickjs-wasm}"
wasm_path="${WASIX_EDGEJS_WASM:-${edgejs_root}/build-wasix/edgejs.wasm}"
guest_root="${WASIX_EDGEJS_GUEST_ROOT:-/workspace}"
guest_test_tmp_root="${WASIX_EDGEJS_GUEST_TEST_TMP_ROOT:-/tmp/edgejs-node-test}"
workspace_dirs_csv="${WASIX_EDGEJS_WORKSPACE_DIRS:-test,lib,deps,assets,build-quickjs-wasix}"
guest_exec_path="${WASIX_EDGEJS_GUEST_EXEC_PATH:-${guest_root}/build-quickjs-wasix/edgejs.wasm}"
# WASIX builds read builtin JS and ICU data from the filesystem instead of
# embedding them (EDGE_EMBED_BUILTIN_SOURCES/EDGE_EMBED_ICU_DATA=OFF). The build
# scripts stage both trees next to edgejs.wasm; see
# scripts/stage_wasix_static_data.py.
#
# napi runner: the staged trees beside WASIX_EDGEJS_WASM are passed explicitly
# (--builtin-js-dir mounts at /node-lib; ICU is mounted at /usr/share/edge-icu).
# wasmer runner: the package's wasmer.toml [fs] maps /node-lib and
# /usr/share/edge-icu. A package [fs] entry wins over a --volume at the same
# guest path, so explicit overrides are mounted elsewhere and selected through
# EDGEJS_LIB_DIR / EDGE_ICU_DATA instead.
#
# WASIX_EDGEJS_NODE_LIB / WASIX_EDGEJS_ICU_DATA override the host directories.
wasm_build_dir="$(dirname "${wasm_path}")"
node_lib_override="${WASIX_EDGEJS_NODE_LIB:-}"
icu_data_override="${WASIX_EDGEJS_ICU_DATA:-}"
guest_icu_data_dir="/usr/share/edge-icu"
guest_override_root="/edgejs-static-data-override"
# QuickJS's guest-stack guard needs enough host stack to raise a JS exception.
# Wasmer's 1 MiB default can trap first with LLVM/amd64; retain the override.
wasmer_stack_args=(--stack-size "${WASMER_STACK_SIZE:-4194304}")
created_run_root=0

# Extra `wasmer run` flags for package-based lanes. Word-split intentionally.
wasmer_extra_args=()
if [[ -n "${WASMER_EXTRA_ARGS:-}" ]]; then
  # shellcheck disable=SC2206
  wasmer_extra_args+=(${WASMER_EXTRA_ARGS})
fi

if [[ -n "${WASIX_EDGEJS_HOST_RUN_ROOT:-}" ]]; then
  host_run_root="${WASIX_EDGEJS_HOST_RUN_ROOT}"
else
  host_run_root="$(mktemp -d "${TMPDIR:-/tmp}/edgejs-wasix-node.XXXXXX")"
  created_run_root=1
fi

host_workspace_root="${host_run_root}/workspace"
host_test_tmp_root="${host_run_root}/node-test"
mkdir -p "${host_workspace_root}" "${host_test_tmp_root}"

cleanup() {
  local status=$?
  if [[ "${WASIX_EDGEJS_KEEP_TMP:-0}" == "1" ]]; then
    printf 'Preserving WASIX EdgeJS test run root: %s\n' "${host_run_root}" >&2
  elif [[ "${WASIX_EDGEJS_KEEP_TMP:-0}" == "failed" && "${status}" != "0" ]]; then
    printf 'Preserving failed WASIX EdgeJS test run root: %s\n' "${host_run_root}" >&2
  elif [[ "${created_run_root}" == "1" ]]; then
    rm -rf "${host_run_root}"
  fi
  return "${status}"
}
trap cleanup EXIT

test_serial_input="${EDGEJS_WASIX_TEST_ID:-$*}"
test_serial_hash="$(printf '%s' "${test_serial_input}" | cksum | awk '{print $1}')"
test_serial_id="${TEST_SERIAL_ID:-wasix-${test_serial_hash}-$$}"

rewrite_guest_path_arg() {
  local arg="$1"
  case "${arg}" in
    "${edgejs_root}"/*)
      printf '%s\n' "${guest_root}/${arg#"${edgejs_root}/"}"
      ;;
    "${edgejs_root}")
      printf '%s\n' "${guest_root}"
      ;;
    *=${edgejs_root}/*)
      local key="${arg%%=*}"
      local value="${arg#*=}"
      printf '%s\n' "${key}=${guest_root}/${value#"${edgejs_root}/"}"
      ;;
    *=${edgejs_root})
      local key="${arg%%=*}"
      printf '%s\n' "${key}=${guest_root}"
      ;;
    *)
      printf '%s\n' "${arg}"
      ;;
  esac
}

guest_args=()
for arg in "$@"; do
  guest_args+=("$(rewrite_guest_path_arg "${arg}")")
done

mount_specs=(
  "${host_workspace_root}:${guest_root}"
  "${host_test_tmp_root}:${guest_test_tmp_root}"
  "${edgejs_root}/ssl-certs:/usr/local/ssl"
)

if [[ -d "${package_dir}/etc" ]]; then
  mount_specs+=("${package_dir}/etc:/etc")
fi

IFS=',' read -r -a workspace_dirs <<< "${workspace_dirs_csv}"
for workspace_dir in "${workspace_dirs[@]}"; do
  workspace_dir="${workspace_dir#"${workspace_dir%%[![:space:]]*}"}"
  workspace_dir="${workspace_dir%"${workspace_dir##*[![:space:]]}"}"
  [[ -z "${workspace_dir}" ]] && continue
  if [[ ! -d "${edgejs_root}/${workspace_dir}" ]]; then
    printf 'warning: skipping missing WASIX EdgeJS workspace dir: %s\n' "${edgejs_root}/${workspace_dir}" >&2
    continue
  fi
  mount_specs+=("${edgejs_root}/${workspace_dir}:${guest_root}/${workspace_dir}")
done

env_specs=(
  "HOME=/tmp"
  "EDGE_EXEC_PATH=${guest_exec_path}"
  "NODE_TEST_DIR=${guest_test_tmp_root}"
  "TEST_SERIAL_ID=${test_serial_id}"
)

if [[ -n "${EDGE_TRACE_TTY:-}" ]]; then
  env_specs+=("EDGE_TRACE_TTY=${EDGE_TRACE_TTY}")
fi

if [[ "${runner_kind}" == "napi" ]]; then
  node_lib_dir="${node_lib_override:-${wasm_build_dir}/node-lib}"
  icu_data_dir="${icu_data_override:-${wasm_build_dir}/icu}"
  if [[ ! -d "${node_lib_dir}" || ! -d "${icu_data_dir}" ]]; then
    printf 'error: staged WASIX static data missing (node-lib: %s, icu: %s); rebuild with ./wasix/build-wasix.sh\n' \
      "${node_lib_dir}" "${icu_data_dir}" >&2
    exit 1
  fi
  mount_specs+=("${icu_data_dir}:${guest_icu_data_dir}")
  runner_args=(
    "${runner_bin}"
    "${wasm_path}"
    --program-name edge
    --builtin-js-dir "${node_lib_dir}"
    --cwd "${guest_root}"
  )
  for env_spec in "${env_specs[@]}"; do
    runner_args+=(--env "${env_spec}")
  done
  for mount_spec in "${mount_specs[@]}"; do
    runner_args+=(--mount "${mount_spec}")
  done
  runner_args+=(-- "${guest_args[@]}")

  if [[ "${WASIX_EDGEJS_TRACE:-0}" == "1" ]]; then
    printf '%q ' "${runner_args[@]}" >&2
    printf '\n' >&2
  fi

  "${runner_args[@]}"
  exit $?
fi

if [[ -n "${node_lib_override}" ]]; then
  mount_specs+=("${node_lib_override}:${guest_override_root}/node-lib")
  env_specs+=("EDGEJS_LIB_DIR=${guest_override_root}/node-lib")
fi
if [[ -n "${icu_data_override}" ]]; then
  mount_specs+=("${icu_data_override}:${guest_override_root}/icu")
  env_specs+=("EDGE_ICU_DATA=${guest_override_root}/icu")
fi

wasmer_env_args=()
for env_spec in "${env_specs[@]}"; do
  wasmer_env_args+=(--env "${env_spec}")
done

volume_args=()
for mount_spec in "${mount_specs[@]}"; do
  volume_args+=(--volume "${mount_spec}")
done

if [[ "${WASIX_EDGEJS_TRACE:-0}" == "1" ]]; then
  {
    printf '%q ' "${runner_bin}" run \
      --llvm \
      "${wasmer_extra_args[@]+"${wasmer_extra_args[@]}"}" \
      "${wasmer_stack_args[@]+"${wasmer_stack_args[@]}"}" \
      --net \
      "${wasmer_env_args[@]}" \
      "${volume_args[@]}" \
      --cwd "${guest_root}" \
      "${package_dir}" \
      -- "${guest_args[@]}"
    printf '\n'
  } >&2
fi

"${runner_bin}" run \
  --llvm \
  "${wasmer_extra_args[@]+"${wasmer_extra_args[@]}"}" \
  "${wasmer_stack_args[@]+"${wasmer_stack_args[@]}"}" \
  --net \
  "${wasmer_env_args[@]}" \
  "${volume_args[@]}" \
  --cwd "${guest_root}" \
  "${package_dir}" \
  -- "${guest_args[@]}"
