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

## Investigation: LLVM versus Cranelift

User requested an explanation for the lower LLVM result. First establish
repeatability: the original compiler groups ran sequentially, with separate
native executable images and short warmups. Use one LLVM-enabled runner with
explicit runtime backend selection and compiler-specific caches; compare the
unchanged current guest in balanced alternating order, with longer warmups.
Record per-process CPU cost and individual measurements. Stop builds during
timing. Profile each backend separately under the same load, excluding
profiling overhead from reported throughput. Inspect concrete compiler
defaults/code generation before changing optimization options. If a gap
persists, vary one supported option at a time and rerun the paired checks.

Ownership: primary owns runner selection, benchmark/profiling harness,
measurements and this note. Integration-review worker audits compiler source
and proposes narrow experiments read-only, with no builds during timing.
Preserve both original result sets and their exact source identities.

### Controlled backend comparison

Added explicit `WASMER_NAPI_COMPILER=llvm|cranelift` selection to an
LLVM-enabled runner. Both choices use the same native image and the selected
compiler's own cache directory. Unknown values and unavailable LLVM fail
explicitly. Neither backend's optimization defaults changed.

Ran six fresh-server pairs with 10-second warmups and 15-second measurements,
32 connections and no pipelining. Pair order alternates LLVM/Cranelift and
Cranelift/LLVM. Both caches were compiled/warmed before timing; task-owned
builds and other benchmarks were stopped during these windows. Host desktop activity was not
disabled; `pmset -g therm` reported no recorded thermal/performance warnings.

| Pair | LLVM req/s | Cranelift req/s | LLVM change |
| --- | ---: | ---: | ---: |
| 1 | 43,789.87 | 42,808.54 | +2.29% |
| 2 | 39,960.54 | 39,504.00 | +1.16% |
| 3 | 41,697.07 | 40,103.47 | +3.97% |
| 4 | 41,630.94 | 41,517.87 | +0.27% |
| 5 | 41,673.60 | 41,586.14 | +0.21% |
| 6 | 42,249.60 | 40,116.27 | +5.32% |

LLVM wins all six pairs. Overall medians are **41,685.335 LLVM** and
**40,817.07 Cranelift** req/s. Median server CPU cost is 34.948 and 35.536
microseconds/request respectively. All twelve measured runs have zero HTTP
errors, timeouts, non-2xx responses or body mismatches.

The median paired log-ratio estimates LLVM **1.72% faster**. A deterministic
10,000-resample paired percentile bootstrap gives a 95% interval of
**+0.24% to +4.64%**. Six pairs on one desktop are a small sample, and this
interval does not remove systematic bias or establish a general speedup.
The original 4.88% LLVM slowdown does not reproduce in this experiment.

Reproduction uses `benchmarks/bench-http-compilers.py`, with raw local data
in `benchmarks/results/http-compiler-paired-2026-11-06.json`. Exact runner
SHA-256: `b8712f173c2f0dcbbdad868e7c361cd80465adbe613466976c8a3d60e663f447`.
The guest, Wasmer revision `a021148`, V8 11.9.9 and LLVM 23.1.4 are unchanged.

### Source audit

At Wasmer `a021148` (compiler code identical to `4434ce0`):

- Both backends use generic AArch64 target settings, rather than an Apple M5
  scheduling model. NaN canonicalization is disabled in both.
- The guest's budgeted tunables delegate to `BaseTunables`, which chooses
  static guarded memory. LLVM uses volatile accesses for this memory and
  inserts a volatile pre-load before ordinary stores to preserve partial-store
  trap behavior (`compiler-llvm/src/translator/code.rs`, access annotations
  around 1453 and stores around 1579). Cranelift lacks that pre-load.
- LLVM enables M0 by default: a hidden heap-base argument with an entry
  fallback and imported-call ABI dispatch. Cranelift uses a single import-call
  path. Disabling M0 is a safe diagnostic option; relaxing volatile accesses
  changes documented trap semantics and is unsuitable as an unqualified fix.
- LLVM's aggressive target-machine setting uses a custom function IR pass
  list, not a full default O3 pipeline. Each Wasm function is compiled in a
  separate LLVM module, preventing cross-function inlining in this path.

These are concrete code-generation differences, not measured causes of the
original HTTP delta. No optimizer or memory-semantics changes were made.
Changing options requires a fresh cache root: cached code is keyed by Wasm
hash and compiler directory, not all current optimization settings.

### Separate profiles and legacy-image diagnostics

Captured `sample` profiles for each backend in separate fresh-server runs:
10-second warmup, 20 seconds of client load and a 10-second/1 ms sampling
window inside that load. Profiling throughput is excluded from the comparison.
Both sampled loads had zero HTTP errors. Raw files are under
`/private/tmp/edgejs-http-llvm-tools/profiles/`.

The execution thread is `TokioTaskManager Thread Pool_thread_1`; the main
thread and most other workers are parked. Exclusive leaf samples on that
thread reconcile to 4,628 Cranelift and 4,721 LLVM snapshots:

| Recognizable symbol family | Cranelift samples | LLVM samples |
| --- | ---: | ---: |
| Native V8 / builtins | 745 (16.10%) | 773 (16.37%) |
| Rust/C++ N-API | 791 (17.09%) | 837 (17.73%) |
| WASIX / virtual I/O | 289 (6.24%) | 258 (5.46%) |
| Socket send/receive | 753 (16.27%) | 788 (16.69%) |
| Wasmer / coroutine / store machinery | 715 (15.45%) | 660 (13.98%) |
| Other named/system code | 708 (15.30%) | 773 (16.37%) |
| Unresolved PCs | 627 (13.55%) | 632 (13.39%) |

These are sampled stack-residence shares, **not CPU utilization percentages**.
Unresolved PCs include both Wasm and V8 JavaScript JIT code, so exact guest
ownership cannot be assigned. Both profiles show substantial shared host
work: N-API/store bookkeeping, native V8 and socket I/O. LLVM compiles only
the Wasm guest; JavaScript continues to use the same V8 engine. That limits
the relevance of optimizing guest instructions for this small HTTP handler.
Neither profile establishes a compiler-specific bottleneck or explains a
roughly 2% throughput delta.

The current guest has DWARF but no Wasm name section. Wasmer's perf-map emitter
only records named functions, so enabling it alone would not resolve these
guest PCs. No artifact-format changes were made for profiling.

Two additional attempts to repeat the original separate-executable comparison
encountered unrelated CMake/clang activity on the desktop. A one-second
process monitor caught the second overlap. Both legacy-image diagnostics are
excluded from inference; files are
`benchmarks/results/http-compiler-old-images{-recheck}-2026-11-06.json` and
`/private/tmp/edgejs-http-llvm-tools/old-images-recheck-monitor.json`.
No unrelated processes were stopped. The six-pair comparison preceded these
observed bursts, but its desktop-activity caveat remains.

Conclusion: the initial short, grouped 4.88% slowdown is not a reliable
compiler comparison. The same-executable, balanced, longer-warmup experiment
puts LLVM slightly ahead. The data does not identify which initial confound
(warmup, ordering, native executable layout or ambient activity) caused that
historical number. Preserve it as an initial measurement, not a demonstrated
LLVM regression. No unsafe optimization setting was enabled to alter results.

### Guest-only control

A supplementary WAT guest imports only WASI `clock_time_get` and `fd_write`:
no JavaScript or N-API calls. It times a dependent integer recurrence and a
cyclic linear-memory read/modify/write loop inside the guest, excluding startup
and compilation. Outputs include checksums and a post-timing full-memory
digest, preventing removal of the work. Four balanced measured pairs follow
one warmup pair on the same runtime-selection executable.

| Kernel | Work | LLVM median | Cranelift median | Paired LLVM duration change |
| --- | --- | ---: | ---: | ---: |
| Integer recurrence | 1 billion iterations | 1.5830255 s | 1.763624 s | -10.20% |
| Linear memory | 300 million operations, 1 MiB | 0.413924 s | 0.415226 s | -0.40%; mixed pair signs |

All ten processes return identical checksums: integer `28da266f33491738`,
memory `e45951a4c6bbb05f`. A clean startup check and 21 one-second process
snapshots observe no clang/rustc/CMake/Ninja activity. This is a narrow
code-generation control, not an additional Edge HTTP or Node comparison.
It supports a workload-dependent compiler benefit, rather than a universal
LLVM slowdown. Source, driver, binary records and monitored results are under
`/private/tmp/edgejs-http-llvm-tools/compute-control/`; the portable investigation
summary retains the actual WAT source and duration samples.

### Shipping and verification

N-API `7ac28da0e76f3e12c7090c05dde7968453198559` contains the runtime
compiler selector. The measured runner was built from that source change;
the native host image, Wasmer dependency and optimizer defaults are identical
across each measured pair. Both `cli` and `cli-llvm` Release builds pass.
The full Edge public realm fixture passes with the ordinary Cranelift build
and with each runtime selection in the LLVM-enabled build. Invalid compiler
values and LLVM requests in builds without LLVM fail explicitly.

The updated provider pin, balanced-pair harness and portable investigation
summary are included in Edge PR 179 / N-API PR 140. The initial measurements
remain recorded, with the stronger result and experimental limits above.
