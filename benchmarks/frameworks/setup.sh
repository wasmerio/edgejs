#!/usr/bin/env bash
# Builds the framework benchmark fixtures.
#
# usage: benchmarks/frameworks/setup.sh [build-dir]
#
# Copies each fixture to <build-dir> (default: benchmarks/frameworks/.build),
# installs its locked dependencies with `npm ci`, and builds the Next.js
# standalone server. The result is the `fixtures` directory that
# bench-framework-wasix-hosts.py expects. Rerunning it rebuilds from scratch.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
build="${1:-$here/.build}"
mkdir -p "$build"
build="$(cd "$build" && pwd)"

command -v npm >/dev/null || { echo "setup.sh: npm is required" >&2; exit 1; }

for fixture in http express hono next tools; do
  rm -rf "${build:?}/$fixture"
  mkdir -p "$build/$fixture"
  (cd "$here/$fixture" && tar -cf - --exclude node_modules --exclude .next .) |
    (cd "$build/$fixture" && tar -xf -)
done

for fixture in express hono tools next; do
  echo "setup.sh: installing $fixture" >&2
  (cd "$build/$fixture" && npm ci --no-audit --no-fund --loglevel=error)
done

echo "setup.sh: building the Next.js standalone server" >&2
(cd "$build/next" && NEXT_TELEMETRY_DISABLED=1 npm run --silent build)

test -f "$build/next/.next/standalone/server.js"
test -f "$build/tools/node_modules/autocannon/autocannon.js"
echo "$build"
