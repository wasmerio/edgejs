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

### `empty-startup`

Runs an empty script.

What it isolates:
- process startup overhead for a no-op workload

What it does not measure:
- useful application work
- async scheduling
- parsing or serialization cost

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

## Build Edge locally

```bash
make build
```

## Example benchmark command

```bash
EDGE_BIN=./build-edge/edge NODE_BIN=node BUN_BIN=bun DENO_BIN=deno ./benchmarks/run.sh console-log
```
