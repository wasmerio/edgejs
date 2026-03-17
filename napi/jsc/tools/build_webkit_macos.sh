#!/usr/bin/env bash
set -euo pipefail

default_webkit_root="/Users/syrusakbary/Development/WebKit"
if [[ "${HOME:-}" != "/Users/syrusakbary" ]]; then
  default_webkit_root="${HOME}/Development/WebKit"
fi

webkit_root="${WEBKIT_ROOT:-$default_webkit_root}"
webkit_url="${WEBKIT_GIT_URL:-https://github.com/WebKit/WebKit.git}"
webkit_configuration="${WEBKIT_CONFIGURATION:-Release}"
webkit_clone_flags=()
if [[ -n "${WEBKIT_CLONE_FLAGS:-}" ]]; then
  read -r -a webkit_clone_flags <<< "${WEBKIT_CLONE_FLAGS}"
else
  webkit_clone_flags=(--depth 1 --single-branch --filter=blob:none --no-tags)
fi
webkit_build_flag="--release"
if [[ "${webkit_configuration}" == "Debug" ]]; then
  webkit_build_flag="--debug"
fi

log() {
  printf '[build_webkit_macos] %s\n' "$*"
}

fail() {
  log "$*"
  exit 1
}

if [[ "$(uname -s)" != "Darwin" ]]; then
  fail "This script only supports macOS."
fi

command -v git >/dev/null || fail "git is required."
command -v xcodebuild >/dev/null || fail "xcodebuild is required."

if [[ ! -d "${webkit_root}" ]]; then
  mkdir -p "$(dirname "${webkit_root}")"
  log "Cloning WebKit into ${webkit_root} (${webkit_clone_flags[*]})"
  git clone "${webkit_clone_flags[@]}" "${webkit_url}" "${webkit_root}"
elif [[ ! -d "${webkit_root}/.git" ]]; then
  fail "${webkit_root} exists but is not a git checkout."
else
  if [[ "${WEBKIT_UPDATE:-1}" == "1" ]]; then
    if [[ -n "$(git -C "${webkit_root}" status --porcelain)" ]]; then
      log "Skipping git pull because ${webkit_root} has local changes."
    else
      log "Updating WebKit checkout in ${webkit_root}"
      git -C "${webkit_root}" pull --ff-only
    fi
  fi
fi

build_script="${webkit_root}/Tools/Scripts/build-webkit"
if [[ -d "${webkit_root}/.git" && ! -e "${build_script}" ]]; then
  log "Restoring interrupted WebKit checkout at ${webkit_root}"
  git -C "${webkit_root}" restore --source=HEAD --staged --worktree :/
fi
[[ -x "${build_script}" ]] || fail "Missing WebKit build script at ${build_script}"

log "Building WebKit (${webkit_configuration})"
"${build_script}" "${webkit_build_flag}"

product_dir="${webkit_root}/WebKitBuild/${webkit_configuration}"
framework_binary="${product_dir}/JavaScriptCore.framework/Versions/A/JavaScriptCore"
framework_headers="${product_dir}/JavaScriptCore.framework/Headers"
[[ -f "${framework_binary}" ]] || fail "Missing JavaScriptCore framework binary at ${framework_binary}"
[[ -d "${framework_headers}" ]] || fail "Missing JavaScriptCore framework headers at ${framework_headers}"

webkit_commit="$(git -C "${webkit_root}" rev-parse HEAD)"

log "WebKit root: ${webkit_root}"
log "WebKit commit: ${webkit_commit}"
log "JavaScriptCore headers: ${framework_headers}"
log "JavaScriptCore library: ${framework_binary}"
