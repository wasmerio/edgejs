#!/usr/bin/env bash
#
# Provision the Wasmer runtime pieces edgejs CI consumes (the C-API dist that
# native builds link, and the CLI that runs the WASIX tests), selectable via
# repo-level GitHub Actions variables so no code change is needed to flip
# modes. Three modes, first match wins:
#
#   1. WASMER_SOURCE_REF       Build wasmer from source at this git ref of
#                              WASMER_SOURCE_REPO (default wasmerio/wasmer).
#                              Accepts a full commit SHA (preferred: immutable
#                              cache key), a branch, a tag, or "pull/N/head".
#                              Native jobs build the C-API dist
#                              (make build-capi package-capi); the WASIX job
#                              builds the CLI (make build-wasmer) with the
#                              same prebuilt LLVM wasmer's own CI uses, since
#                              the WASIX runner scripts pass --llvm.
#   2. WASMER_ARTIFACT_RUN_ID  Download the prebuilt dist from that run of
#                              wasmer's "Builds" workflow (same artifacts as a
#                              release). Requires the WASMER_ARTIFACTS_TOKEN
#                              secret (a token with actions:read on the wasmer
#                              repo; the default GITHUB_TOKEN is repo-scoped).
#                              Note: those artifacts expire after ~2 days.
#   3. (default)               Released artifacts, as before: CMake downloads
#                              the C-API tarball from GitHub releases and the
#                              WASIX job installs the CLI from get.wasmer.io.
#                              WASMER_RELEASE_VERSION overrides the pinned
#                              version for both.
#
# Usage:
#   provision-wasmer.sh resolve <native|wasix>   emit mode + cache key to
#                                                $GITHUB_OUTPUT (cache key only
#                                                in source mode)
#   provision-wasmer.sh native                   provide the C-API dist and
#                                                export EDGE_QUICKJS_WASMER_DIST_ROOT
#   provision-wasmer.sh wasix                    put a wasmer CLI on $GITHUB_PATH
#
# Source-mode outputs land in .wasmer-provision/ (cached by resolved SHA via
# actions/cache); the wasmer checkout itself lives in .wasmer-src/ and is not
# cached. In source mode the WASIX job intentionally keeps release headers for
# compiling edge (wasm.h declares the full standard C API; symbols resolve at
# runtime through the host bridge) — only the CLI is built from source.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$REPO_ROOT"

PROVISION_DIR="$REPO_ROOT/.wasmer-provision"
SRC_DIR="$REPO_ROOT/.wasmer-src"
WASMER_SOURCE_REPO="${WASMER_SOURCE_REPO:-wasmerio/wasmer}"
RELEASE_CLI_DEFAULT="v7.2.0"
# Same prebuilt LLVM wasmer's Builds workflow installs before build-wasmer.
LLVM_URL_LINUX_AMD64="https://github.com/wasmerio/llvm-custom-builds/releases/download/22.x/llvm-linux-amd64.tar.xz"

github_env() { echo "$1" >> "${GITHUB_ENV:-/dev/null}"; }
github_path() { echo "$1" >> "${GITHUB_PATH:-/dev/null}"; }
github_output() { echo "$1" >> "${GITHUB_OUTPUT:-/dev/null}"; }

current_mode() {
  if [[ -n "${WASMER_SOURCE_REF:-}" ]]; then
    echo source
  elif [[ -n "${WASMER_ARTIFACT_RUN_ID:-}" ]]; then
    echo artifact
  else
    echo release
  fi
}

resolve_sha() {
  local ref="$WASMER_SOURCE_REF"
  if [[ "$ref" =~ ^[0-9a-fA-F]{40}$ ]]; then
    echo "$ref"
    return
  fi
  local url="https://github.com/${WASMER_SOURCE_REPO}.git"
  local sha
  sha="$(git ls-remote "$url" "$ref" "refs/heads/$ref" "refs/tags/$ref" | head -n1 | cut -f1)"
  if [[ -z "$sha" ]]; then
    echo "provision-wasmer: cannot resolve WASMER_SOURCE_REF '$ref' in $url" >&2
    exit 1
  fi
  echo "$sha"
}

host_artifact_name() {
  case "$(uname -s)-$(uname -m)" in
    Linux-x86_64) echo wasmer-linux-amd64 ;;
    Linux-aarch64) echo wasmer-linux-aarch64 ;;
    Darwin-arm64) echo wasmer-darwin-arm64 ;;
    Darwin-x86_64) echo wasmer-darwin-amd64 ;;
    *)
      echo "provision-wasmer: unsupported host $(uname -s)-$(uname -m)" >&2
      exit 1
      ;;
  esac
}

fetch_source() {
  local sha="${WASMER_RESOLVED_SHA:-$(resolve_sha)}"
  if [[ -d "$SRC_DIR/.git" && "$(git -C "$SRC_DIR" rev-parse HEAD 2>/dev/null)" == "$sha" ]]; then
    return
  fi
  rm -rf "$SRC_DIR"
  mkdir -p "$SRC_DIR"
  git -C "$SRC_DIR" init -q
  git -C "$SRC_DIR" remote add origin "https://github.com/${WASMER_SOURCE_REPO}.git"
  git -C "$SRC_DIR" fetch --depth 1 origin "$sha"
  git -C "$SRC_DIR" checkout -q FETCH_HEAD
  # lib/napi is a workspace member, so cargo needs it present even for
  # builds that don't enable the napi features. The test-suite submodules
  # are not needed.
  git -C "$SRC_DIR" -c protocol.version=2 submodule update --init --depth 1 lib/napi
}

# Setup mirrored from wasmer's own Builds workflow (.github/workflows/build.yml
# in wasmerio/wasmer): ninja on both platforms, automake + SDKROOT from the
# CommandLineTools SDK on macOS (SDKROOT is exported per-invocation instead of
# switching xcode-select globally, so the edgejs build later in the job keeps
# the default toolchain). Rust itself comes from rustup honoring wasmer's
# rust-toolchain.toml.
install_build_deps() {
  case "$(uname -s)" in
    Linux)
      if ! command -v ninja >/dev/null; then
        sudo apt-get update -qq && sudo apt-get install -y -qq ninja-build
      fi
      ;;
    Darwin)
      brew list ninja >/dev/null 2>&1 || brew install ninja
      brew list automake >/dev/null 2>&1 || brew install automake
      WASMER_BUILD_SDKROOT="$(xcrun --sdk macosx --show-sdk-path)"
      export WASMER_BUILD_SDKROOT
      ;;
  esac
  if ! command -v rustup >/dev/null; then
    curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh -s -- -y --no-modify-path
    # shellcheck disable=SC1091
    source "$HOME/.cargo/env"
  fi
}

run_wasmer_make() {
  if [[ "$(uname -s)" == Darwin && -n "${WASMER_BUILD_SDKROOT:-}" ]]; then
    (cd "$SRC_DIR" && SDKROOT="$WASMER_BUILD_SDKROOT" make "$@")
  else
    (cd "$SRC_DIR" && make "$@")
  fi
}

provision_native_source() {
  local dist="$PROVISION_DIR/dist"
  if [[ -f "$dist/lib/libwasmer.a" && -f "$dist/include/wasmer.h" ]]; then
    echo "provision-wasmer: reusing cached C-API dist at $dist"
  else
    install_build_deps
    fetch_source
    # capi_compiler_features filters out llvm in wasmer's Makefile, matching
    # the released C-API artifacts (cranelift), so no LLVM is needed here.
    run_wasmer_make build-capi package-capi ENABLE_LLVM=0 ENABLE_V8=0 ENABLE_NAPI_V8=0
    rm -rf "$dist"
    mkdir -p "$PROVISION_DIR"
    cp -R "$SRC_DIR/package" "$dist"
  fi
  github_env "EDGE_QUICKJS_WASMER_DIST_ROOT=$dist"
  echo "provision-wasmer: EDGE_QUICKJS_WASMER_DIST_ROOT=$dist"
}

provision_wasix_source() {
  local bin_dir="$PROVISION_DIR/bin"
  if [[ -x "$bin_dir/wasmer" ]]; then
    echo "provision-wasmer: reusing cached CLI at $bin_dir/wasmer"
  else
    install_build_deps
    fetch_source
    # The WASIX runner scripts invoke `wasmer run --llvm`, so the CLI must be
    # built with the LLVM backend, using the same prebuilt LLVM wasmer CI uses.
    if [[ "$(uname -s)-$(uname -m)" != "Linux-x86_64" ]]; then
      echo "provision-wasmer: no prebuilt LLVM mapping for this host; add one to provision-wasmer.sh" >&2
      exit 1
    fi
    local llvm_dir="$SRC_DIR/.llvm"
    if [[ ! -x "$llvm_dir/bin/llvm-config" ]]; then
      mkdir -p "$llvm_dir"
      curl --retry 3 --proto '=https' --tlsv1.2 -sSfL "$LLVM_URL_LINUX_AMD64" | tar xJ -C "$llvm_dir"
    fi
    # wasm-c-api (the wasm_c_api_v0 bridge) is always included by the
    # Makefile. NAPI-V8 (the `wasmer run --experimental-napi` runtime the
    # v8-wasix lane needs) is opt-in via EDGE_WASMER_NAPI=1; the quickjs jobs
    # don't exercise it and skip the extra V8 build time.
    local napi_enabled=0
    if [[ "${EDGE_WASMER_NAPI:-0}" == "1" ]]; then
      napi_enabled=1
    fi
    (cd "$SRC_DIR" && PATH="$llvm_dir/bin:$PATH" \
      ENABLE_LLVM=1 ENABLE_V8=0 ENABLE_NAPI_V8=$napi_enabled make build-wasmer)
    mkdir -p "$bin_dir"
    cp "$SRC_DIR/target/release/wasmer" "$bin_dir/wasmer"
  fi
  github_path "$bin_dir"
  "$bin_dir/wasmer" --version
}

download_artifact() {
  local artifact_root="$PROVISION_DIR/artifact/root"
  if [[ -d "$artifact_root" ]]; then
    echo "$artifact_root"
    return
  fi
  if [[ -z "${GH_TOKEN:-}" ]]; then
    echo "provision-wasmer: artifact mode needs the WASMER_ARTIFACTS_TOKEN secret (actions:read on ${WASMER_SOURCE_REPO}) exported as GH_TOKEN" >&2
    exit 1
  fi
  local name raw extract
  name="$(host_artifact_name)"
  raw="$PROVISION_DIR/artifact/raw"
  extract="$PROVISION_DIR/artifact/extract"
  rm -rf "$PROVISION_DIR/artifact"
  mkdir -p "$raw" "$extract"
  gh run download "$WASMER_ARTIFACT_RUN_ID" -R "$WASMER_SOURCE_REPO" -n "$name" -D "$raw" 1>&2
  find "$raw" -name '*.tar.gz' -exec tar xzf {} -C "$extract" \;
  local header
  header="$(find "$extract" "$raw" -path '*/include/wasmer.h' 2>/dev/null | head -n1)"
  if [[ -z "$header" ]]; then
    echo "provision-wasmer: artifact $name from run $WASMER_ARTIFACT_RUN_ID contains no include/wasmer.h" >&2
    exit 1
  fi
  mv "$(dirname "$(dirname "$header")")" "$artifact_root"
  echo "$artifact_root"
}

provision_native_artifact() {
  local root
  root="$(download_artifact)"
  if [[ ! -f "$root/lib/libwasmer.a" ]]; then
    echo "provision-wasmer: artifact dist has no lib/libwasmer.a" >&2
    exit 1
  fi
  github_env "EDGE_QUICKJS_WASMER_DIST_ROOT=$root"
  echo "provision-wasmer: EDGE_QUICKJS_WASMER_DIST_ROOT=$root"
}

provision_wasix_artifact() {
  local root
  root="$(download_artifact)"
  if [[ ! -f "$root/bin/wasmer" ]]; then
    echo "provision-wasmer: artifact dist has no bin/wasmer" >&2
    exit 1
  fi
  chmod +x "$root/bin/wasmer"
  # Keep headers in sync with the CLI for the WASIX compile too.
  github_env "EDGE_QUICKJS_WASMER_DIST_ROOT=$root"
  github_path "$root/bin"
  "$root/bin/wasmer" --version
}

provision_native_release() {
  if [[ -n "${WASMER_RELEASE_VERSION:-}" ]]; then
    github_env "EDGE_QUICKJS_WASMER_VERSION=$WASMER_RELEASE_VERSION"
    echo "provision-wasmer: EDGE_QUICKJS_WASMER_VERSION=$WASMER_RELEASE_VERSION"
  else
    echo "provision-wasmer: release mode, CMake default pins apply"
  fi
}

provision_wasix_release() {
  local version="${WASMER_RELEASE_VERSION:-$RELEASE_CLI_DEFAULT}"
  curl https://get.wasmer.io -sSfL | sh -s "$version"
  github_path "$HOME/.wasmer/bin"
  "$HOME/.wasmer/bin/wasmer" --version
}

cmd_resolve() {
  local role="${1:?usage: provision-wasmer.sh resolve <native|wasix>}"
  local mode
  mode="$(current_mode)"
  github_output "mode=$mode"
  echo "provision-wasmer: mode=$mode"
  if [[ "$mode" == source ]]; then
    local sha variant=""
    sha="$(resolve_sha)"
    github_env "WASMER_RESOLVED_SHA=$sha"
    # A napi-enabled CLI is a different artifact than the plain one; keep the
    # cache entries separate.
    if [[ "${EDGE_WASMER_NAPI:-0}" == "1" ]]; then
      variant="-napi"
    fi
    github_output "cache_key=wasmer-provision-v1-${role}${variant}-$(uname -s)-$(uname -m)-${sha}"
    echo "provision-wasmer: ${WASMER_SOURCE_REPO}@${WASMER_SOURCE_REF} -> $sha"
  fi
}

case "${1:-}" in
  resolve)
    cmd_resolve "${2:-}"
    ;;
  native)
    case "$(current_mode)" in
      source) provision_native_source ;;
      artifact) provision_native_artifact ;;
      release) provision_native_release ;;
    esac
    ;;
  wasix)
    case "$(current_mode)" in
      source) provision_wasix_source ;;
      artifact) provision_wasix_artifact ;;
      release) provision_wasix_release ;;
    esac
    ;;
  *)
    echo "usage: provision-wasmer.sh <resolve <role>|native|wasix>" >&2
    exit 1
    ;;
esac
