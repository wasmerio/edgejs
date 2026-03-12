#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
DEPS_DIR="${PROJECT_ROOT}/deps"

clone_or_update() {
  local path="$1"
  local url="$2"
  local branch="$3"

  if [[ -e "${path}" && ! -d "${path}/.git" ]]; then
    echo "error: ${path} exists but is not a git repository" >&2
    exit 1
  fi

  if [[ ! -d "${path}/.git" ]]; then
    echo "Cloning ${url} (${branch}) -> ${path}"
    git clone --depth 1 --branch "${branch}" "${url}" "${path}"
    return
  fi

  echo "Updating ${path} (${branch})"
  git -C "${path}" remote set-url origin "${url}"
  git -C "${path}" fetch --depth 1 origin "${branch}"
  if git -C "${path}" show-ref --verify --quiet "refs/heads/${branch}"; then
    git -C "${path}" checkout "${branch}"
  else
    git -C "${path}" checkout -b "${branch}" "origin/${branch}"
  fi
  if ! git -C "${path}" merge --ff-only "origin/${branch}"; then
    echo "warning: ${path} has local changes; skipping fast-forward merge" >&2
  fi
}

mkdir -p "${DEPS_DIR}"
# Keep the upstream branch name until the external repo renames it.
clone_or_update "${DEPS_DIR}/libuv-wasix" "https://github.com/wasix-org/libuv.git" "ubi"
clone_or_update "${DEPS_DIR}/openssl-wasix" "https://github.com/wasix-org/openssl.git" "master"

echo "WASIX deps are ready under ${DEPS_DIR}"
