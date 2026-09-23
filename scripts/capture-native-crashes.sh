#!/usr/bin/env bash
set -euo pipefail

binary="${1:?usage: capture-native-crashes.sh BINARY CRASH_DIRECTORY}"
crash_dir="${2:?usage: capture-native-crashes.sh BINARY CRASH_DIRECTORY}"
mkdir -p "${crash_dir}"

# Only symbolic backtraces are uploaded. Core files contain process memory and
# remain temporary inputs to LLDB on the same CI runner.
shopt -s nullglob
cores=()
# Linux's %e may contain the crashing thread's name, not "edge".
for candidate in "${crash_dir}"/core.*; do
  if [[ -f "${candidate}" && "${candidate}" != *.txt ]]; then
    cores+=("${candidate}")
  fi
done
if (( ${#cores[@]} == 0 )); then
  printf 'No Edge core dump was produced by the failed test step.\n' \
    > "${crash_dir}/no-core.txt"
  exit 0
fi

for core in "${cores[@]}"; do
  report="${crash_dir}/$(basename "${core}").txt"
  timeout 60s lldb --batch --file "${binary}" --core "${core}" \
    -o 'thread backtrace all' -o 'image list' > "${report}" 2>&1 || true
  rm -f -- "${core}"
  cat "${report}"
done
