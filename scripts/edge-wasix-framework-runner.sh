#!/usr/bin/env bash
set -euo pipefail

# Node-compatible launcher used by scripts/framework-test.js to run framework
# workloads through either a WASIX package or the raw imported-N-API module.
#
# The harness symlinks node_modules/.bin/node to this script and always invokes
# framework commands with cwd set to the project directory.

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

resolve_edgejs_root() {
  if [[ -n "${EDGEJS_ROOT:-}" ]]; then
    printf '%s\n' "${EDGEJS_ROOT}"
    return 0
  fi

  if [[ -f "${script_dir}/../quickjs-wasm/wasmer.toml" ]]; then
    printf '%s\n' "$(cd "${script_dir}/.." && pwd)"
    return 0
  fi

  local dir
  dir="$(pwd -P)"
  while [[ "${dir}" != "/" ]]; do
    if [[ -f "${dir}/quickjs-wasm/wasmer.toml" ]]; then
      printf '%s\n' "${dir}"
      return 0
    fi
    dir="$(dirname "${dir}")"
  done

  printf 'error: could not locate EdgeJS root (set EDGEJS_ROOT)\n' >&2
  return 1
}

edgejs_root="$(resolve_edgejs_root)"
runner_kind="${WASIX_RUNNER_KIND:-wasmer}"
runner_bin="${WASIX_RUNNER_BIN:-${WASMER_BIN:-wasmer}}"
package_dir="${WASIX_EDGEJS_PACKAGE_DIR:-${edgejs_root}/quickjs-wasm}"
wasm_path="${WASIX_EDGEJS_WASM:-${edgejs_root}/build-wasix/edgejs.wasm}"
guest_app_root="${WASIX_FRAMEWORK_GUEST_ROOT:-/app}"
# QuickJS's guest-stack guard needs enough host stack to raise a JS exception.
# Wasmer's 1 MiB default can trap first with LLVM/amd64; retain the override.
wasmer_stack_args=(--stack-size "${WASMER_STACK_SIZE:-4194304}")

# Extra `wasmer run` flags for package-based lanes. Word-split intentionally.
wasmer_extra_args=()
if [[ -n "${WASMER_EXTRA_ARGS:-}" ]]; then
  # shellcheck disable=SC2206
  wasmer_extra_args+=(${WASMER_EXTRA_ARGS})
fi

app_root="$(pwd -P)"
while [[ ! -d "${app_root}/node_modules" && "${app_root}" != "/" ]]; do
  app_root="$(dirname "${app_root}")"
done
if [[ ! -d "${app_root}/node_modules" ]]; then
  printf 'error: expected framework project root with node_modules: %s\n' "$(pwd -P)" >&2
  exit 1
fi

entry_root="$(pwd -P)"
guest_cwd="${guest_app_root}"
if [[ "${entry_root}" == "${app_root}"/* ]]; then
  guest_cwd="${guest_app_root}/${entry_root#"${app_root}/"}"
elif [[ "${entry_root}" != "${app_root}" ]]; then
  printf 'error: standalone entry cwd is outside framework project root: %s\n' "${entry_root}" >&2
  exit 1
fi

env_specs=("HOME=/tmp")
for env_name in PORT HOST HOSTNAME STATIC_ROOT NODE_ENV; do
  if [[ -n "${!env_name:-}" ]]; then
    env_specs+=("${env_name}=${!env_name}")
  fi
done

rewrite_guest_path_arg() {
  local arg="$1"
  case "${arg}" in
    "${app_root}"/*)
      printf '%s\n' "${guest_app_root}/${arg#"${app_root}/"}"
      ;;
    "${app_root}")
      printf '%s\n' "${guest_app_root}"
      ;;
    "${edgejs_root}"/*)
      printf '%s\n' "/workspace/${arg#"${edgejs_root}/"}"
      ;;
    "${edgejs_root}")
      printf '%s\n' "/workspace"
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
  "${app_root}:${guest_app_root}"
  "${edgejs_root}/ssl-certs:/usr/local/ssl"
)

if [[ -d "${package_dir}/etc" ]]; then
  mount_specs+=("${package_dir}/etc:/etc")
fi

if [[ "${runner_kind}" == "napi" ]]; then
  runner_args=(
    "${runner_bin}"
    "${wasm_path}"
    --program-name edge
    --builtin-js-dir "${edgejs_root}/lib"
    --cwd "${guest_cwd}"
  )
  for env_spec in "${env_specs[@]}"; do
    runner_args+=(--env "${env_spec}")
  done
  runner_args+=(--mount "${edgejs_root}:/workspace")
  for mount_spec in "${mount_specs[@]}"; do
    runner_args+=(--mount "${mount_spec}")
  done
  runner_args+=(-- "${guest_args[@]}")

  if [[ "${WASIX_EDGEJS_TRACE:-0}" == "1" ]]; then
    printf '%q ' "${runner_args[@]}" >&2
    printf '\n' >&2
  fi

  exec "${runner_args[@]}"
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
      --cwd "${guest_cwd}" \
      "${package_dir}" \
      -- "${guest_args[@]}"
    printf '\n'
  } >&2
fi

exec "${runner_bin}" run \
  --llvm \
  "${wasmer_extra_args[@]+"${wasmer_extra_args[@]}"}" \
  "${wasmer_stack_args[@]+"${wasmer_stack_args[@]}"}" \
  --net \
  "${wasmer_env_args[@]}" \
  "${volume_args[@]}" \
  --cwd "${guest_cwd}" \
  "${package_dir}" \
  -- "${guest_args[@]}"
