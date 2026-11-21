# Edge.js Benchmarks

This directory contains small standalone benchmark workloads for comparing Edge.js against other runtimes using the same JavaScript files.

The benchmark files are intentionally simple and focused. They are meant to be easy to run, easy to inspect, and easy to extend.

## Goals

- Keep workloads small and reproducible
- Compare the same JavaScript workload across runtimes
- Use whole-process wall-clock timing for one-shot workloads
- Preserve simple startup baselines while expanding toward more representative runtime tasks
- Keep interpretation cautious and avoid broad claims from tiny benchmarks

## Current workloads

### `text-encoder-encode-into`

Runs eight warmed batches of 8,888 `TextEncoder.encodeInto()` calls using short
ASCII and Unicode fragments, a shared encoder, and a reused destination. This
isolates encoding binding overhead encountered by streamed Astro templates.
It reports per-batch milliseconds and a deterministic encoded-byte checksum;
each batch has checksum `88868`. It excludes startup, HTTP transport, and
destination allocation. The investigation's HTTP fixture used a different
16-character ASCII input and read-plus-written checksum `284416`; its numbers must not be compared
with this mixed-input workload as though they were the same benchmark.

Run the same file with each runtime or guest build being compared:

```sh
node benchmarks/workloads/text-encoder-encode-into.js
./build-edge/edge benchmarks/workloads/text-encoder-encode-into.js
```

For WASIX comparisons, execute the file through the same Wasmer/Edge host and
compiler configuration for both guest builds. Use Release builds, prewarm the
guest, keep cgroup settings identical, and avoid simultaneous compiler jobs.

### `empty-startup`

Runs an empty script.

What it isolates:
- process startup overhead for a no-op workload

What it does not measure:
- useful application work
- async scheduling
- parsing or serialization cost

### `cli-eval-empty`

Runs an empty `-e` snippet.

What it isolates:
- CLI eval startup overhead without file loading
- argument parsing and eval-entry bootstrap cost

What it does not measure:
- file-entry startup
- useful application work
- richer module-loader or inspector setup

### `cli-print-literal`

Runs `-p "1"`.

What it isolates:
- CLI print startup cost for a trivial expression
- eval-path setup plus minimal output work

What it does not measure:
- file-entry startup
- module-heavy eval behavior
- non-trivial compute

### `cli-print-process-version`

Runs `-p "process.version"`.

What it isolates:
- CLI print startup for a small process-backed expression
- startup behavior closer to common “version probe” checks

What it does not measure:
- file-entry startup
- require-heavy eval behavior
- async or I/O work

### `console-log`

Prints a single line to stdout.

What it isolates:
- startup cost plus trivial execution and stdout handling

What it does not measure:
- realistic application throughput
- non-trivial compute or async runtime behavior

### `json-parse-stringify`

Repeatedly parses and re-serializes the same JSON payload and prints a deterministic checksum.

What it isolates:
- short-lived JSON parsing and serialization work in a one-shot process

What it does not measure:
- long-running server throughput
- I/O-heavy application behavior
- large real-world document variability

### `promise-microtask-chain`

Builds and awaits a deterministic promise chain, then prints a checksum.

What it isolates:
- promise scheduling and microtask processing in a small one-shot workload

What it does not measure:
- timer behavior
- network or filesystem async work
- framework-level reactivity behavior

### `zlib-gzip-gunzip-sync`

Repeatedly compresses and decompresses the same in-memory payload with `node:zlib` and prints a deterministic checksum.

What it isolates:
- short-lived compression and decompression cost for a one-shot process
- `node:zlib` compatibility on a small deterministic workload
- runtime behavior for a common built-in module beyond startup-only baselines

What it does not measure:
- streaming compression behavior
- filesystem or network I/O
- large real-world archive throughput
- long-running server workloads

### `string-compare-split`

Repeatedly splits the same delimited string and performs deterministic string comparisons over the resulting parts, then prints a checksum.

What it isolates:
- short-lived string splitting cost
- simple string comparison work on repeated in-memory inputs
- a runtime surface that maps to the roadmap’s string optimization lane

What it does not measure:
- regex-heavy parsing
- large-text search workloads
- filesystem or network I/O
- long-running application throughput

### `http-loopback`
Starts a small local HTTP server, performs a fixed number of loopback requests against it, and prints a deterministic checksum.

What it isolates:
- local HTTP request/response overhead in a small one-shot process
- runtime behavior across a basic built-in networking path
- a more representative server/client surface than startup-only baselines

What it does not measure:
- real network conditions
- TLS/HTTPS behavior
- production throughput
- websocket behavior
- framework or router overhead

### `http-server-throughput`

Serves a fixed 12-byte response over HTTP/1.1 without request logging. The
separate `bench-http-throughput.py` harness compares two WASIX guests on the
same N-API host, using Autocannon with 32 keep-alive connections, no pipelining,
a 3-second warmup and three alternating 10-second measurements per guest.
It validates status/body and rejects runs with HTTP errors or timeouts. The
output includes raw measurements, medians and artifact SHA-256 hashes.

```sh
python3 benchmarks/bench-http-throughput.py \
  --runner build-wasix-napi/target/release/napi_wasmer \
  --published-wasm /tmp/edgejs-published/modules/edge \
  --current-wasm build-wasix/edgejs.wasm \
  --builtin-dir build-wasix/node-lib \
  --autocannon /tmp/edgejs-http-benchmark-tools/node_modules/.bin/autocannon \
  --output benchmarks/results/http-throughput.json
```

`--builtin-dir` must be the staged `build-wasix/node-lib`, not the repo's
`lib/`. The current WASIX guest reads its builtins from `/node-lib` and checks
the catalog digest written there by `wasix/build-wasix.sh`. These harnesses do
not mount ICU data at `/usr/share/edge-icu`.

Download/unpack the exact baseline version with `wasmer package download`
first; install Autocannon 9.0.0 in the indicated isolated prefix. Build the
current guest and warm both compilation caches before collecting timed samples;
avoid concurrent builds or benchmarks. This measures guest-version throughput with the provider
held constant; it excludes startup, compilation, TLS and framework overhead.

For LLVM, build the N-API runner with `--features cli-llvm` and LLVM 23.1
(`LLVM_SYS_231_PREFIX` points to that installation). See `napi/docs/cli.md`.
Pass that binary as `--runner` and use a fresh `WASMER_NAPI_CACHE_DIR` when
changing Wasmer revisions. The CLI separates LLVM and Cranelift cache entries.

To compare the compilers themselves, use one LLVM-enabled executable and
one guest with the balanced-pair harness:

```sh
WASMER_NAPI_CACHE_DIR=/tmp/edgejs-compiler-comparison \
python3 benchmarks/bench-http-compilers.py \
  --runner build-wasix-napi/target/release/napi_wasmer \
  --wasm build-wasix/edgejs.wasm --builtin-dir build-wasix/node-lib \
  --autocannon /tmp/edgejs-http-benchmark-tools/node_modules/.bin/autocannon \
  --output benchmarks/results/http-compilers.json
```

It selects `WASMER_NAPI_COMPILER` for each fresh server, defaults to six
balanced pairs with 10-second warmups and 15-second measurements, and records
throughput, server CPU cost and a paired bootstrap interval. Small desktop
samples still need cautious interpretation. Prewarm both compiler caches and
keep builds stopped; use a fresh cache root when changing optimization options.

### `timers-settimeout-chain`

Chains 200 sequential `setTimeout(fn, 0)` calls. Each callback fires, increments a counter, and schedules the next timer. Prints a deterministic checksum when the chain completes.

What it isolates:
- per-`setTimeout` dispatch cost in the event loop timer phase
- callback scheduling and execution overhead across N iterations
- a runtime surface that maps to the roadmap's timers benchmark lane

What it does not measure:
- concurrent timer scheduling (all timers here are sequential)
- high-resolution timer behavior
- timer cancellation or re-scheduling
- `setImmediate` or `queueMicrotask` (covered by `promise-microtask-chain`)

## Framework servers on WASIX

`bench-framework-wasix-hosts.py` measures what a change does to real servers:
`node:http`, Express 5.2.1, Hono 4.12.31 and Next.js 16.1.7 (standalone),
each running inside the WASIX guest on the host N-API runner and answering
`GET /api/bench` with the 11-byte body `hello edge\n`. It compares "lanes",
each a `napi_wasmer` runner plus an Edge.js guest build, so it covers changes
to Edge.js, to N-API and to Wasmer alike.

Prerequisites: Node.js and npm on the host, `hyperfine`, and a WASIX build
(`make build-wasix-napi`).

```sh
# Once: install and build the fixtures (locked versions, needs network).
benchmarks/frameworks/setup.sh

# Before the change: save the current build as the baseline.
python3 benchmarks/bench-framework-wasix-hosts.py snapshot baseline

# Make the change and rebuild (make build-wasix-napi), then:
python3 benchmarks/bench-framework-wasix-hosts.py smoke
python3 benchmarks/bench-framework-wasix-hosts.py run
```

`smoke` starts every server once per lane, sends 2,000 requests and checks
every reply. It takes a few seconds and also flags server logs that mention a
store-lending error, a V8 check failure or a Rust panic. Its req/s figures
are unwarmed and only a sanity check.

`run` is the benchmark. For each fixture and lane, a fresh server is started
and warmed up for 3 seconds with 32 connections, then hyperfine times a fixed
number of requests (250,000 for `node:http` and Hono, 150,000 for Express,
20,000 for Next.js) through `bench-framework-load.cjs`, which validates every
reply and records exact timing and latency percentiles. It does this for two
rounds of two runs each, rotating fixture order and reversing lane order
between rounds, then times process start to the first reply. It prints a
table with each lane's change against the first lane and writes everything
(artifact hashes, raw runs, summary) to
`benchmarks/results/framework-wasix-hosts.json`. On an idle machine a full
run of two lanes takes about 10 minutes.

Useful options:

- `--amount-scale 0.2` (before `run`) with `run --rounds 1` gives a rough
  answer in about a minute.
- `run --fixtures http hono` limits the servers.
- `run --idle 0` turns off the idle-machine gate. By default each cell waits
  for three seconds of at least 85% idle CPU and is redone (up to three
  attempts) when the machine was busy afterwards or its two runs differ by
  more than 6%. On a busy desktop the gate can wait a long time.
- `run --startup-runs 0` skips the first-reply pass.

The default config is `bench-framework-wasix-hosts.example.json`: the
`baseline` snapshot against the current `build-wasix/` and
`build-wasix-napi/` outputs. For other comparisons, write your own config
(files named `benchmarks/frameworks/*.local.json` are ignored by git) and
pass `--config`. Paths in a config are relative to the config file. A lane
takes one of:

- `{"snapshot": "NAME"}`: a build saved with `snapshot NAME`.
- `{"build": "DIR", "runner": "PATH"}`: a `build-wasix`-style directory
  (`edgejs.wasm`, `node-lib/`, `icu/`) and a `napi_wasmer` binary.
- `{"runner", "guest", "builtins", "mounts"}` spelled out.

and optionally `"env"` (guest environment) and `"host_env"` (runner
environment, for example `{"WASMER_NAPI_COMPILER": "llvm"}`). `snapshot`
takes `--build` and `--runner` to save something other than the default
outputs.

### On pull requests

The `framework-bench` job in `.github/workflows/test-and-build.yml` runs this
benchmark on every pull request from a branch of this repository, after the
`v8-wasix` job has built the PR. It downloads that build (`edge-wasix` and
`napi-wasmer-linux-amd64`) and the newest finished build of the base branch,
runs both at the same time on one runner, each pinned to half of its CPUs and
swapping halves between rounds, and posts the comparison as a pull request
comment. Later pushes update the same comment, which keeps a short table of
earlier runs. The benchmark itself is capped at about 100 seconds, so it uses
10% of the request counts above: it catches large changes, and smaller ones
need a local run. `benchmarks/ci/framework-bench.py` holds the logic; its
`run` and `comment` subcommands also work locally on two directories that
each hold `dist/` (an unpacked `edge-wasix.zip`) and `runner/napi_wasmer`.

Interpret small differences cautiously: on a laptop, run-to-run noise is
about 3 to 5%, larger for Next.js. Re-run, or use more rounds, before
trusting a gap of that size. Servers run with `EDGE_BYTECODE_CACHE=0` and
the Cranelift compiler unless the config says otherwise.

## Coverage notes

Each benchmark in this directory is intentionally narrow.

A single workload should be read as one slice of a runtime surface, not as a complete representation of that entire area. For example, a one-shot in-memory zlib benchmark is useful for covering one compression path, but it does not stand in for streaming behavior, decompression cost, larger payloads, or broader application throughput.

The goal of this suite is to grow coverage incrementally with small, reviewable workloads that are easy to reproduce and compare across runtimes. When a surface needs deeper coverage, it should expand as a small family of related benchmarks rather than one oversized mixed workload.

## Runtime prerequisites

Install and verify the comparison runtimes you want to use:

- locally built Edge.js
- Node.js
- Bun
- Deno
- `hyperfine`

## Verify comparison runtimes

```bash
./build-edge/edge benchmarks/workloads/console-log.js
node benchmarks/workloads/console-log.js
bun benchmarks/workloads/console-log.js
deno run benchmarks/workloads/console-log.js
```

## Deno comparator notes

Deno is included as a comparator to broaden the cross-runtime matrix beyond Edge.js, Node.js, and Bun.

The benchmark harness runs Deno workloads with:

```bash
deno run benchmarks/workloads/<workload>.js
```

These benchmark files are intentionally small and standalone so they can be compared across runtimes with minimal harness-specific behavior.

CLI startup benchmarks intentionally omit Deno from the matrix because they exercise Node/Bun-compatible `-e` and `-p` entry paths rather than file-entry workloads.

Each benchmark run also attempts to capture an Edge-only startup phase profile by running the Edge command with `EDGE_STARTUP_PROFILE=1`. Alongside the usual `hyperfine` exports, the harness writes:

- `benchmarks/results/<workload>.edge-profile.json`
- `benchmarks/results/<workload>.edge-profile.md`

These artifacts record the native startup phase breakdown for the exact Edge command used in the benchmark, so wall-clock deltas can be reviewed together with phase deltas. If startup profile JSON is not emitted on your checkout, the harness writes a placeholder note instead.

## Build Edge locally

```bash
make build
```

To check whether the current local binary emits startup profile output:

```bash
EDGE_STARTUP_PROFILE=1 ./build-edge/edge -e ""
```

Some checkouts may require additional build support for startup profiling. A common rebuild attempt is:

```bash
cmake -S . -B build-edge -DCMAKE_BUILD_TYPE=Release -DEDGE_ENABLE_STARTUP_PROFILE=ON
cmake --build build-edge -j8
```

## Example benchmark command

```bash
EDGE_BIN=./build-edge/edge NODE_BIN=node BUN_BIN=bun DENO_BIN=deno ./benchmarks/run.sh console-log
```

```bash
EDGE_BIN=./build-edge/edge \
NODE_BIN=/Users/syrusakbary/.nvm/versions/node/v22.22.2/bin/node \
BUN_BIN=/opt/homebrew/bin/bun \
./benchmarks/run.sh cli-print-literal
```
