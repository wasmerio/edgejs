#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
default_edgejs_root="$(cd "${script_dir}/.." && pwd)"

edgejs_root="${EDGEJS_ROOT:-${default_edgejs_root}}"
wasmer_bin="${WASMER_BIN:-wasmer}"
package_dir="${WASIX_EDGEJS_PACKAGE_DIR:-${edgejs_root}/quickjs-wasm}"
guest_root="${WASIX_EDGEJS_GUEST_ROOT:-/workspace}"

guest_args=()
for arg in "$@"; do
  case "${arg}" in
    "${edgejs_root}"/*)
      guest_args+=("${guest_root}/${arg#"${edgejs_root}/"}")
      ;;
    "${edgejs_root}")
      guest_args+=("${guest_root}")
      ;;
    *)
      guest_args+=("${arg}")
      ;;
  esac
done

exec "${wasmer_bin}" run \
  --net \
  --env HOME=/tmp \
  --env "NODE_TEST_DIR=${guest_root}/test" \
  --volume "${edgejs_root}:${guest_root}" \
  --cwd "${guest_root}" \
  "${package_dir}" \
  -- "${guest_args[@]}"
