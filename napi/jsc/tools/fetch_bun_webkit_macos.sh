#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd "${script_dir}/../../.." && pwd)"

default_tag="$(bash "${script_dir}/bun_webkit_release_tag.sh")"
tag="${BUN_WEBKIT_TAG:-$default_tag}"

arch="$(uname -m)"
case "${arch}" in
  arm64) asset_arch="arm64" ;;
  x86_64) asset_arch="amd64" ;;
  *) printf '[fetch_bun_webkit_macos] Unsupported macOS architecture: %s\n' "${arch}" >&2; exit 1 ;;
esac

default_root="${repo_root}/.ci/jsc/${tag}/macos-${asset_arch}"
if [[ -z "${GITHUB_WORKSPACE:-}" ]]; then
  default_root="/Users/syrusakbary/Development/bun-webkit/${tag}"
  if [[ "${HOME:-}" != "/Users/syrusakbary" ]]; then
    default_root="${HOME}/Development/bun-webkit/${tag}"
  fi
fi

destination_root="${BUN_WEBKIT_ROOT:-$default_root}"
asset_name="bun-webkit-macos-${asset_arch}.tar.gz"
download_url="https://github.com/oven-sh/WebKit/releases/download/${tag}/${asset_name}"

log() {
  printf '[fetch_bun_webkit_macos] %s\n' "$*"
}

fail() {
  log "$*"
  exit 1
}

if [[ "$(uname -s)" != "Darwin" ]]; then
  fail "This script only supports macOS."
fi

if [[ -f "${destination_root}/lib/libJavaScriptCore.a" && -d "${destination_root}/include" ]]; then
  log "Using existing Bun WebKit SDK at ${destination_root}"
  exit 0
fi

tmpdir="$(mktemp -d)"
trap 'rm -rf "${tmpdir}"' EXIT
archive="${tmpdir}/${asset_name}"

mkdir -p "$(dirname "${destination_root}")"
rm -rf "${destination_root}"
mkdir -p "${destination_root}"

log "Downloading ${download_url}"
curl -L --fail --output "${archive}" "${download_url}"

log "Extracting into ${destination_root}"
tar -xzf "${archive}" -C "${destination_root}" --strip-components=1

[[ -d "${destination_root}/include/JavaScriptCore" ]] || fail "Missing headers after extraction"
[[ -f "${destination_root}/lib/libJavaScriptCore.a" ]] || fail "Missing libJavaScriptCore.a after extraction"
[[ -f "${destination_root}/lib/libWTF.a" ]] || fail "Missing libWTF.a after extraction"
[[ -f "${destination_root}/lib/libbmalloc.a" ]] || fail "Missing libbmalloc.a after extraction"
[[ -x "${destination_root}/bin/jsc" ]] || fail "Missing jsc binary after extraction"

log "Installed Bun WebKit SDK at ${destination_root}"
