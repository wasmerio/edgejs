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

## Build Edge locally

```bash
make build