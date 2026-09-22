# EdgeJS integration validation

Status: local validation complete; exact-commit CI pending. Owner: EdgeJS worker.
Write ownership: EdgeJS runtime/tests/build changes if necessary and this note;
no N-API or QuickJS writes, no other plan edits. Audit CI/build dependencies first,
initialize non-NAPI submodules safely, then build native QuickJS and WASIX and run
runtime checks. Reproduce exact failures and inspect LLDB before semantic fixes.
Coordinator owns commits/gitlinks/pushes/PRs. Other workers may be active.

## Environment and validation scope

- Isolated worktree: `/private/tmp/quickjs-upstream-sync/edgejs`.
- macOS arm64; CMake/Ninja/Apple Clang available. Wasmer 7.4.2,
  wasixcc 0.4.4 (LLVM 21.1.2), Node 26.0.0, pnpm 11.2.2 installed.
- Initializing `test`, `ssl-certs`, `deps/libuv-wasix`,
  `deps/openssl-wasix`, and `wasmer-examples` only. The nested N-API/QuickJS
  worktrees must not be passed to `git submodule update`.
- Native build: `make build-edge-quickjs-cli CMAKE_BUILD_TYPE=Release JOBS=4`.
- Native runtime: `make test-quickjs-only TEST_JOBS=4`, Intl tests, then focused
  engine-sensitive smoke tests and framework/standalone verification where practical.
- WASIX rebuild must run from `quickjs-wasm/` as `./build.sh`, then package
  validation, safe-mode smoke tests, and relevant runtime checks.

## Existing CI coverage caveat

The baseline Makefile lists `test-quickjs-lang` in `.PHONY` but defines no recipe;
`QUICKJS_LANG_TESTS` is also undefined. The QuickJS workflow invokes this target,
so its language step currently executes no tests. This is pre-existing and not
evidence of successful language validation. Direct smoke coverage will be recorded
separately. No source changes have been made for this caveat.

## Preliminary results (before consumed-fork ancestry audit)

Native Release and WASIX builds completed successfully, including the WASIX
no-N-API-import check and package validation. Native CTest passed 2/2; native
and WASIX Intl targets completed (the ICU environment test skips as too old).
The Next standalone app passed both host Node and native Edge stages.

A temporary engine-sensitive smoke script passed against Node, the existing
Edge binary, and the newly built native binary. It covers ArrayBuffer transfer
and detachment, structured clone, V8-shaped serialization, vm cached bytecode,
AsyncLocalStorage across Promise/microtask boundaries, and fetch/HTTP.

WASIX safe-mode tests passed after one retry: the first cold-cache invocation
emitted a Wasmer warning about removing a nonexistent corrupt cache entry, which
the smoke script correctly rejected as unexpected stderr. The warm-cache retry
passed all seven cases. This was not a guest assertion/crash.

These are preliminary results: the coordinator subsequently discovered that
N-API main consumes an additional unmerged QuickJS Error-stack branch and is
integrating it. Rebuild and rerun engine-dependent checks after that signal.
Logs are under `/private/tmp/quickjs-upstream-sync/edge-*.log`.

The preliminary native Node compatibility run completed with **1,741/1,741
passing** in 2m47s. This run predates the consumed-fork Error-stack merge, and is
not a claim about the final engine commit.

## Final local validation

After integrating the consumed QuickJS Error-stack branch, final dependencies
are QuickJS `0daca74` and N-API `54d7989`.

- Native Release Edge rebuild: passed.
- WASIX Edge rebuild from `quickjs-wasm/` using `./build.sh`: passed, including
  no-N-API-import verification.
- Engine-sensitive native smoke: passed (transfer/detachment, structured clone,
  serialization, vm bytecode, AsyncLocalStorage, Promise/microtask ordering, HTTP).
- Native console/diagnostics-channel categories with existing CI exclusions:
  **77/77 passed**.
- Direct native Error checks: stack-frame hiding, prepareStackTrace, aggregate
  errors, formatList, and decorated error stacks passed.
- Final WASIX safe-mode smoke: **7/7 passed** on warm-cache retry. The first
  invocation for each new artifact produced the same host Wasmer cold-cache
  warning noted above. No guest failure was observed.
- Final WASIX Intl target completed; ICU environment case skips as too old.
- Final `wasmer package build --check quickjs-wasm`: passed.
- Final WASIX `test/parallel/test-x509-escaping.js`, via
  `scripts/edge-wasix-node-runner.sh`: **passed (exit 0, empty output)** on macOS
  arm64 with Wasmer 7.4.2.

`test-errors-systemerror.js`, run directly with `--expose-internals`, still
expects V8's wording for access to an undefined object's property; QuickJS says
`cannot read property 'syscall' of undefined`. The existing Edge binary fails
the same assertion, so this is pre-existing. The Node harness defaults to
skipping files whose first meaningful line is `// Flags:`, including this case.
No broad engine-error-message workaround was added.

## CI follow-up

The first integration CI head, `c55d9e07`, predated the consumed Error-stack merge.
Its QuickJS native Linux/macOS jobs passed. Its WASIX job reported **1,674 passes
and 1 failure**, `test-x509-escaping.js` with `RuntimeError: call stack exhausted`.
The final integration head `4c81f11` repeated the exact WASIX failure while
all native QuickJS checks passed. Both released Wasmer 7.3.0 and 7.4.2 pass the
exact candidate test in native Linux ARM64 Ubuntu Docker. CI uses custom Wasmer
`5281e55` and Linux AMD64, so this does not reproduce its environment. Local
wasixcc is 0.4.4; CI remains 0.4.3/sysroot `v2026-07-30.1`.

The user requested Wasmer 7.4.2 in CI if it works better. Compatibility review
found no dependency on the custom Wasmer C-API extensions. The Actions source
variable was removed and `WASMER_RELEASE_VERSION=v7.4.2` set through the existing
provisioning interface; the next full QuickJS CI matrix validates that release.
No engine guard, TLS, or test-exclusion changes were added.

Final V8 Linux initially reported 1,748 passes and one Buffer SIGBUS. The V8
subtree is identical to Edge main's pin; a fresh local V8 11.9.9 build passed
100 mixed Buffer/HTTP2 runs. The full final V8 workflow passed on attempt two.
This does not claim a root cause or fix for that intermittent signal.

The full final native/WASIX/framework CI matrices are the integration gate. The
full preliminary native suite was not duplicated locally after the Error-stack
merge because focused final stack checks and exact-commit CI cover that change.
No Edge runtime, test, or build source changes were required during this subtask.
The subsequent AMD64 investigation below changes the package launchers' host
stack budget.

## AMD64 host-stack correction

The same candidate X509 test traps with released Wasmer 7.4.2 on AMD64 at its
default 1 MiB host stack; it passes at 2 and 4 MiB. The consumed baseline passes
X509 at 1 MiB, but both artifacts trap with a minimal recursive JavaScript
function at that host limit. At 4 MiB, both instead catch the normal QuickJS
`RangeError`: 2,584 calls for the baseline and 3,233 for the candidate.

LLDB reproduced a fault in a protected 4 KiB mapping on a Tokio task thread;
AMD64 emulation prevented a reliable native unwind. Upstream's smaller
linear-memory interpreter frame allows more recursion before the guest guard.
The engine audit found no merge-specific recursion bug. See the detailed
[diagnostic record](../troubleshooting/node-compat/napi/021_quickjs_upstream_sync.md).

The Node and framework package launchers now use a 4 MiB Wasmer host stack by
default, preserving `WASMER_STACK_SIZE` overrides and QuickJS's own 1 MiB guard.
The imported N-API runner path is unchanged. Direct package launch instructions
document the same option because package annotations cannot configure it.
The default Node launcher passed X509 three consecutive times on AMD64, and
shell syntax/diff checks passed. The new full CI matrix validates this change
with Wasmer 7.4.2 and CI's unchanged wasixcc 0.4.3 toolchain.
