# HTTP throughput comparison after the realm upgrade

| Field | Value |
| --- | --- |
| Status | Complete; initial measurements show a lower current median with overlapping run ranges. |
| Request | Open an Edge PR, then compare simple HTTP requests/second against the published `syrusakbary/edgejs` version. |
| PRs | Edge [#179](https://github.com/wasmerio/edgejs/pull/179), N-API [#140](https://github.com/wasmerio/napi/pull/140). |
| Scope | Compare the current imported-N-API WASIX guest with the registry package's unchanged extracted guest on one host/provider. |
| Ownership | Primary worker: benchmark workload, orchestration, raw results and this note. |
| Validation | Exact HTTP status/body, warmup, alternating runs, error counts, artifact hashes and runtime identities. |

The registry resolved `syrusakbary/edgejs` to **0.2.5-gfbc0e8e** on
2026-11-06. Downloaded and validated the exact WebC with Wasmer 7.5.0; extract
its `modules/edge` atom without rebuilding it.

Use the same rebuilt `napi_wasmer` host (N-API `469bf10`, V8 provider,
Cranelift WASIX compiler) for both guests. This isolates the Edge guest
comparison: the provider's N-API revision is deliberately held constant and
the measurement does not independently quantify old-versus-new provider cost.
Native V8/QuickJS executables are not compared to a WASIX baseline.

Workload: HTTP/1.1 on loopback, persistent connections, no pipelining,
12-byte `hello world\n` response, no request logging, 32 concurrent clients.
Warm up each fresh server for 3 seconds; measure 10 seconds; repeat three
times in alternating published/current order. Record each response/error
count, request rate and p50/p99 latency, then report median requests/second.
Use Autocannon 9.0.0 installed in an isolated temporary prefix. Startup and
WebAssembly compilation are excluded. Keep other builds stopped during timed
runs; stop each process group before starting the next server.

## Results

Measured on an Apple M5 Max, arm64 macOS, 18 logical CPUs. Autocannon 9.0.0
runs under Node 26.0.0. The local guest was rebuilt in Release mode from Edge
`71e7a9458969f74af7ede7be964f0359acf46e46` (PR 179), with N-API runtime code
`469bf1011a4b091a7a0f60c12c1a5c5344551574`. The provider is V8 custom build
11.9.9; guest `process.versions.v8` is a build-time placeholder, not the host
V8 version. No local builds or other benchmarks ran during timed samples.

| Version | Trial 1 req/s | Trial 2 req/s | Trial 3 req/s | Median req/s | Range req/s |
| --- | ---: | ---: | ---: | ---: | ---: |
| Published `0.2.5-gfbc0e8e` | 25,716.8 | 25,080.0 | 22,465.6 | **25,080.0** | 22,465.6–25,716.8 |
| Current PR | 24,256.0 | 21,745.6 | 23,620.8 | **23,620.8** | 21,745.6–24,256.0 |

The current median is **5.82% lower**. All six measured runs report zero
errors, timeouts, non-2xx responses and body mismatches. Both versions have
median p50 latency 1 ms and p99 latency 2 ms. The measured windows complete
732,630 published and 696,234 current requests in total.

Run order was published/current, current/published, published/current, with a
fresh server and warmup for every run. A separate three-second Node control
using the same workload, client and concurrency reached 113,034.67 req/s with
zero HTTP errors, providing evidence that the client can exceed the observed
Edge rates. The Node control is a load-generator sanity check, not an
additional stable runtime comparison.

Three runs have substantial overlap and variability; this quick benchmark
does not establish a statistically significant regression or attribute the
delta to any single change. The published guest includes older Edge changes,
and both guests use the **same new N-API provider**. This cannot measure the
independent performance change from the provider upgrade itself.

## Artifacts and reproduction

- Published WebC SHA-256:
  `6ce2b9bea44cb116faa0c869612a156b23d40d2b18d6fb210192e5155d61ff2a`.
- Published unmodified atom SHA-256:
  `f5cbf8c1cd39eef56553a4a4e6c7623bbf90fefc00a8c57c08e322f5504e82ae`.
- Current WASIX atom SHA-256:
  `99636f01b39b4d4f5f77c20610309d1702fd9b338daca050bf5eaea1bc23439f`.
- Full raw samples and commands:
  `benchmarks/results/http-throughput-realms-2026-11-06.json` (local artifact).
- Portable measurement summary:
  `benchmarks/results/http-throughput-realms-2026-11-06-summary.json`.

```sh
wasmer package download syrusakbary/edgejs@0.2.5-gfbc0e8e \
  --validate --unpack -o /private/tmp/edgejs-published-0.2.5-gfbc0e8e.webc
npm install --prefix /private/tmp/edgejs-http-benchmark-tools \
  --no-package-lock --no-save autocannon@9.0.0
python3 benchmarks/bench-http-throughput.py \
  --runner build-wasix-napi/target/release/napi_wasmer \
  --published-wasm /private/tmp/edgejs-published-0.2.5-gfbc0e8e/modules/edge \
  --current-wasm build-wasix/edgejs.wasm --builtin-dir lib \
  --autocannon /private/tmp/edgejs-http-benchmark-tools/node_modules/.bin/autocannon \
  --output benchmarks/results/http-throughput-realms-2026-11-06.json
```

Benchmark code/results are a follow-up commit in PR 179. A later N-API
test-only correction accepts V8's direct exception when a standalone synthetic
evaluation callback throws; it does not change the benchmarked runtime code.
