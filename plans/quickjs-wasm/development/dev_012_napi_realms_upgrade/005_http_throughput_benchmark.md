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

## LLVM follow-up

User requested the same comparison with LLVM, then asked to update Wasmer to
latest because current Wasmer uses LLVM 23. Confirmed main at
`4434ce07535d0322308ed89c4f3407cd46cbbea8` uses `llvm23-1-prefer-static` and
Rust 1.99. Update the N-API standalone source pin and lockfile, align the Rust
toolchain and native concurrency CI, and add an optional `cli-llvm` build
feature selecting `LLVM::default()`. Keep both guest atoms, V8 provider,
workload and load-generator settings unchanged. Separate compiler caches so a
cached Cranelift artifact cannot be mistaken for LLVM output. Compile/warm
both guests before timing, stop builds, then repeat three alternating
measurements and retain validation and hashes. Record LLVM version,
optimization defaults, results and caveats here. The previous Wasmer/Cranelift
results remain historical; updating Wasmer also changes the runtime version.

### LLVM 23 and latest Wasmer results

Built native runners from N-API `201ab932e16fefcd2f55fc61c7276d4df372ff97`,
with Wasmer main `4434ce07535d0322308ed89c4f3407cd46cbbea8`, Rust 1.99,
and the refreshed locked dependencies. LLVM is Homebrew 23.1.4, linked
statically with `LLVM_SYS_231_STRICT_VERSIONING=1`. Wasmer's default LLVM
configuration uses aggressive/O3 optimization and M0. JavaScript still runs
on the same V8 custom build 11.9.9. Both original guest atoms and their hashes
are unchanged. The native build command enables `cli-llvm` for LLVM and
`cli` for Cranelift; see `napi/docs/cli.md`.

Used the same machine, workload and Autocannon settings as above. Both guests
were compiled and warmed before measuring; fresh cache root
`/private/tmp/edgejs-http-llvm-tools/cache` contains separate compiler
subdirectories (LLVM artifacts about 67 MB, Cranelift about 92 MB). LLVM's
six alternating trials preceded Cranelift's six alternating trials. No
builds or other benchmarks ran during those windows.

| Compiler | Guest | Trial 1 req/s | Trial 2 req/s | Trial 3 req/s | Median req/s |
| --- | --- | ---: | ---: | ---: | ---: |
| LLVM 23 | Published | 40,784.0 | 42,022.4 | 41,955.2 | **41,955.2** |
| LLVM 23 | Current | 42,227.2 | 40,950.4 | 39,820.81 | **40,950.4** |
| Cranelift | Published | 41,049.6 | 40,284.81 | 41,785.6 | **41,049.6** |
| Cranelift | Current | 42,547.2 | 43,446.4 | 43,052.8 | **43,052.8** |

All twelve measured runs have zero HTTP errors, timeouts, non-2xx responses
and body mismatches. LLVM's current-versus-published median is 2.39% lower,
with overlapping run ranges. Cranelift's is 4.88% higher. Current LLVM is
4.88% below current Cranelift. This quick sequential comparison shows no
LLVM throughput advantage; three repeats do not support broad performance
claims. Historical Cranelift measurements used older Wasmer and Rust/
dependency versions, so their lower rates cannot be attributed to the
compiler alone.

Raw local samples are `benchmarks/results/http-throughput-{llvm23-latest,
cranelift-latest}-2026-11-06.json`; the committed portable summary is
`benchmarks/results/http-throughput-wasmer-latest-2026-11-06-summary.json`.
Runner SHA-256 values:

- LLVM: `1d0095f2c0b82ae810af111b4ade07c236d6d44c6c62cd09f8bf893770765d05`.
- Cranelift: `01e8bae6d06aa931efd5d51f624b20b5ad32a80f72d7cc40bf7a05178e4f312a`.

Latest-Wasmer native provider validation passes 90 tests, with one existing
ignored test: library, concurrency, foreground waker, guest callbacks/cache,
memory and memory pressure. The complete public realm fixture passes on both
new CLI variants with `--experimental-vm-modules`.

Updating the JavaScript-host bridge additionally requires public nonblocking
store acquisition and new memory-view accessors; issue
[024](../troubleshooting/node-compat/napi/024_async_store_nonblocking_lock.md)
records that work. The native timings above use immutable Wasmer main before
the additional API required by the host-JS bridge.

Final N-API pin `4179e538007b12b4b28cd8f1bc521990e09e0a3b` includes the
adapter and pins Wasmer descendant `a021148`. Final native LLVM build,
90 native tests (one existing ignored), public realm fixture, 40 wasm library
tests and 41 Node host-context tests pass. The additional upstream API has
6/6 native and 2/2 wasm ownership/contention checks, plus a successful Wasmer
CLI build. Timed samples retain their original immutable source identities.
The API addition is [Wasmer PR 7143](https://github.com/wasmerio/wasmer/pull/7143);
its full `make lint` also passes.
