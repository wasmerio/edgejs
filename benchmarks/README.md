# Edge.js Benchmarks

This directory contains a small standalone benchmark harness for comparing
Edge.js against other runtimes using the same JavaScript workloads.

## Goals

- Keep workloads simple and reproducible
- Verify correctness on every run
- Measure whole-process wall-clock time for one-shot benchmarks
- Make it easy to compare `edge` and `node` first, then add more runtimes later

## Current benchmarks

- `empty-startup`: startup cost for an empty script
- `console-log`: startup + trivial execution + stdout handling

## Build Edge locally

```bash
make build