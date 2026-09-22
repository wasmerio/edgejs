# QuickJS upstream synchronization compatibility

| | | Remarks |
| --- | --- | --- |
| **Status** | ▶️ | Integrating upstream 0.17.0 and validating retained embedding behavior. |
| **Severity** | High | Repeated Linux WASIX X509 trap blocks the integration CI gate. |

## Action plan

1. Replace the fork coroutine ownership implementation with upstream's complete
   `cur_gc_obj` / `is_coro` implementation; do not combine GC representations.
2. Adapt ArrayBuffer callbacks and delete the superseded N-API detach workaround.
3. Preserve lazy `prepareStackTrace` with upstream Error internal stack storage.
   Audit found an inherited success-return check (`== 0`, while successful
   `JS_DefinePropertyGetSet` returns 1), causing eager + lazy double formatting.
   Reproduce, correct the success check, and add single-call/capture/setter tests.
4. Validate bundled engine tests under sanitizers and GC stress, N-API Release
   and Debug, and native/WASIX EdgeJS via linked PRs.

## Evidence before the stack correction

Debug qjs, inline evaluation:
`let c=0; Error.prepareStackTrace=()=>++c; const e=new Error(); print(c,e.stack,c)`
observes eager formatting during construction and again on stack access.

Direct qjs invocation of an import-bearing module also faults in
`js_inner_module_linking` with a NULL dependency (LLDB reproduced). The fork
intentionally defers module resolution to its host, so bundled tests use the
`run-test262` driver, which resolves modules explicitly. Determine baseline and
scope before changing the standalone loader; this is not evidence of an EdgeJS
loader regression.

## Integration record

See [task analysis](../../../dev_010_quickjs_upstream_sync/README.md) and its
worker notes for the complete retained/deleted inventory and validation results.

## Local verification

The final merge retains consumed `ff1471c`'s `JS_DefineProperty` / `ret == 1`
non-exotic installation and engine regression tests pass. The incorrect return
check was present on fork master but already fixed on the consumer branch.
N-API Release and Debug pass 95/95 each after the fix. Engine ASAN/UBSAN and GC
stress pass. The standalone qjs import failure was separately reproduced on
origin/master `7b8ab77`, confirming it is pre-existing. Cross-repository CI is
pending; no default branch updates have been made.

## First cross-repository CI checkpoint

QuickJS and N-API final heads `0daca74` and `54d7989` are green across all
checks. Initial EdgeJS commit `c55d9e07` (before retaining ff1471c) passed both
native QuickJS jobs but failed WASIX `test-x509-escaping` with call-stack
exhaustion (1,674 passed / 1 failed). The final local WASIX binary passes that
exact test; rerun the final EdgeJS pin before diagnosing a Linux-only failure.
V8 macOS had an HTTP2 test failure and Linux had an npm registry metadata fetch
failure. No tests were skipped or softened to bypass these gates.

## Final-head Linux WASIX blocker and action plan

EdgeJS `4c81f11` / N-API `54d7989` / QuickJS `0daca74` reproduces
`test-x509-escaping` with `RuntimeError: call stack exhausted` in Linux CI run
`35688862524`, job `106621384408`: 1,674 passed and one failed. This repeats the
initial integration failure and is an unresolved merge gate. Native QuickJS
Linux/macOS are green. The final macOS WASIX binary passed the exact test and
nine additional repetitions; the tenth repetition failed with ECONNRESET,
which is a different failure and does not explain the Linux stack trap.

Before changing runtime code:

1. Reproduce the exact test inside native aarch64 `ubuntu:latest` Docker on the
   arm64 host, then match Linux amd64 CI if architecture changes the outcome.
   Docker was not running; the coordinator reminded the user and launched it.
2. Compare the consumed baseline `ff1471c` with the final engine using the same
   WASIX compiler, Wasmer version, runner, and stack settings.
3. Inspect a native debugger/backtrace and Wasmer diagnostics before semantic
   changes. Audit upstream WASI stack accounting against the removed fork
   guard and distinguish linear-memory stack limits from Wasmer host stacks.
4. Make the narrowest demonstrated correction, preserving upstream history and
   avoiding test exclusions or test-level retries. Rebuild from
   `quickjs-wasm/` with `./build.sh`, rerun the exact test first, then relevant
   native/WASIX verification and all affected PR CI.

Separately, final V8 Linux CI passed 1,748 tests and crashed with SIGBUS in
`test-buffer-bytelength`. The V8 tree is unchanged from Edge main's N-API pin;
the existing [native crash record](../../node-test/017_http2_native_lifecycle_crashes.md)
documents this and the earlier macOS HTTP2 failure as intermittent cases.
A fresh final V8 11.9.9 build passed 100 mixed local repetitions. No cause is
proven for the Linux signal. A failed-job-only CI rerun is active; V8 macOS
and WASIX passed the final commit. No V8 source edits were made.

## Wasmer 7.4.2 CI comparison

The user explicitly requested using Wasmer 7.4.2 in CI if it works better.
The failing QuickJS WASIX job uses custom Wasmer `5281e55` (reports 7.3.0),
while previous local checks used released 7.4.2. The custom commit's build
record exists but its artifacts are no longer retained.

Native Linux aarch64 `ubuntu:latest` Docker runs of the exact final candidate
X509 test passed with both released 7.3.0 and 7.4.2. This does not reproduce
the custom Linux amd64 CI environment. Callback tracing on macOS shows balanced
entries/exits and maximum nesting two; no runaway TLS callback recursion was
observed. No native crash occurred locally for LLDB to inspect.

The QuickJS linear-memory guard remains enabled at 1 MiB. Its unsigned-underflow
guard is the only relevant setup difference from the consumed baseline. Wasmer's
host/coroutine stack is a separate limit, and an unclassified JIT signal may be
reported as StackOverflow. Consequently the trap message alone does not prove
the precise failure mechanism. No engine stack or TLS workaround was added.

Compatibility review found Edge's WebAssembly binding only uses standard
`wasm.h` APIs, not the custom shared-memory/module additions. Wasmer 7.4.2
contains newer LLVM indirect-call, store-reentry, and signal handling fixes,
but no direct stack-limit fix was identified. The V8 WASIX N-API runner uses
its own SDK and does not use this QuickJS provisioner.

To test the requested runtime through the existing provisioning mechanism,
EdgeJS Actions configuration now has `WASMER_RELEASE_VERSION=v7.4.2`; the
previous `WASMER_SOURCE_REF=5281e55dac6c85be91560c4c828d49fd2d5b8b5d` was
removed because source mode takes precedence over release mode. All QuickJS
CI lanes will validate released 7.4.2. Keep CI's wasixcc 0.4.3/sysroot
`v2026-07-30.1` unchanged so this comparison changes only the host runtime;
local candidate/baseline builds use wasixcc 0.4.4 and do not establish exact
compiler parity with CI.

The unchanged final V8 workflow passed on attempt two, including the Linux
runtime, framework, and standalone checks. This does not claim to fix the
unreproduced intermittent SIGBUS. Use PR #154's live checks for the new
Wasmer 7.4.2 matrix; the old custom-runtime failure remains recorded above.
