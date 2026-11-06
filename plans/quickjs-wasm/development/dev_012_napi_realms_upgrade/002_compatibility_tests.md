# N-API realms upgrade: compatibility tests

| Field | Value |
| --- | --- |
| Status | Complete: Node, both native Edge engines, both WASIX variants pass the public fixture; 31/31 N-API QuickJS module-state cases pass. |
| Scope | Edge-owned regression coverage for public vm behavior across V8 and QuickJS. |
| Dependencies | Runtime integration uses N-API main `18209d2`. |
| Write ownership | Test worker: `tests/js/napi-realms.js`, registration in `tests/CMakeLists.txt`, this note, and the coordinated N-API module sandbox fix in `napi/quickjs/src/internal/napi_module_wrap.{h,cc}` with its N-API-owned regression in `napi/tests/runners/test_77_module_wrap_state.cc`. |
| Verification | Run fixture with Node, rebuilt V8 Edge and rebuilt QuickJS Edge. |

Cover distinct intrinsics, globals and sandbox writeback; nested execution;
native callbacks returning principal-realm values; compileFunction parsing
contexts; modules/async behavior where the public API supports it; retained
functions after the sandbox variable is dropped. Test observable Node behavior,
not private N-API implementation details. Other workers may change runtime and
build files concurrently; do not modify their write sets.

## Coverage

`tests/js/napi-realms.js` is registered as `edge.napi_realms` with
`--experimental-vm-modules`. It exercises realm-specific intrinsic identity,
sandbox global read/write isolation, a reusable `vm.Script` run in different
contexts, nested realm execution and restoration after an exception, principal
realm native allocations (`TextEncoder`, `Buffer`, crypto errors), explicit and
default `compileFunction` parsing contexts, retained functions, pending promise
jobs, module creation/link/evaluation and cross-context link rejection.
Function and module caches are consumed in a different realm from their
producer. A native hash consumes a foreign typed-array view whose own layout
properties throw, ensuring the native consumer uses internal slots. Native ESM
and temporary CommonJS imports exercise synthetic module facade integration.

The fixture follows `napi/docs/realms.md`: Edge native functions belong to the
principal realm even when called from a vm realm. It deliberately does not
require QuickJS sandbox writes to appear immediately inside reentrant native
callbacks or after compiled-function calls: the provider documents copy-in and
copy-out sandbox semantics. Own microtask queues and per-context string code
generation restrictions are provider-specific and outside the common fixture.

## Verification

Node v26.0.0 and both rebuilt shipping Edge binaries pass `NAPI_REALMS_OK`,
including function/module caches, native import and CommonJS interop.
The exact standalone command is:

```sh
node --experimental-vm-modules tests/js/napi-realms.js
build-edge/edge --experimental-vm-modules tests/js/napi-realms.js
build-edge-quickjs-cli/edge --experimental-vm-modules tests/js/napi-realms.js
ctest --test-dir build-edge -R '^edge.napi_realms$' --output-on-failure
ctest --test-dir build-edge-quickjs-cli -R '^edge.napi_realms$' --output-on-failure
```

All five commands pass. N-API verification stays in its repository:

```sh
cd napi
cmake --build build-napi-quickjs --target napi_quickjs_test_77_module_wrap_state -j4
build-napi-quickjs/quickjs/tests/napi_quickjs_test_77_module_wrap_state
```

The target passes all 31 tests, including sandbox evaluation and rejected
evaluation writeback through both synchronous and promise-returning APIs.

## QuickJS fresh-sandbox module failure and action plan

The minimal failure is a new `vm.createContext({ seed: 100 })`, followed by
`new vm.SourceTextModule('export const value = seed;', { context })`, linking
and evaluating it: QuickJS rejects with `ReferenceError: seed is not defined`.
Running `vm.runInContext('seed', context)` first hides the failure by copying
the sandbox properties into the realm's engine global.

Reproduced the exact fixture under LLDB, then stopped at
`JS_ThrowReferenceError`. The stack is `JS_GetPropertyInternal` →
`JS_CallInternal` → `js_async_function_resume` → module evaluation →
`napi_module_wrap__::evaluate_unless_terminated` → Edge `ModuleWrapEvaluate`.
The provider evaluates in the right context but omits the sandbox
synchronization already used for scripts and compiled functions.

Before changing the provider, the action plan is:

1. Add a provider-owned regression for a fresh sandbox module, mutations made
   to the sandbox after module construction, and synchronous global writeback.
   Cover both `evaluate` and `evaluate_sync` through their shared path.
2. Synchronize the sandbox in and out around the module evaluation using
   existing `napi_contextify__` helpers, preserving evaluation and termination
   error handling and avoiding unrelated module link/status code.
3. Rebuild QuickJS Edge and rerun the strict public fixture. Run the targeted
   N-API regression from the N-API repository when a test build is available.

The provider fix uses existing sandbox synchronization immediately around
`JS_EvaluateModule`. The new N-API regression failed before the fix with
`rejected: seed is not defined`, and passed afterward. The complete standalone
QuickJS module-state target passes 31/31 tests including the additional
rejected-evaluation regression. The primary worker coordinates this
provider write set and owns Edge module link/status adaptation separately.

## Remaining asynchronous sandbox limitation

Synchronous module writes are copied back even when its evaluation promise is
rejected: `globalThis.written = 1; throw Error('expected')` leaves
`sandbox.written === 1` on all three runtimes. Both public and N-API-owned
regressions cover this behavior.

QuickJS sandbox properties changed after top-level await are not copied out at
promise-job completion. The minimal source is
`await Promise.resolve(); globalThis.written = 2;` in a fresh context whose
sandbox starts with `written: 0`. After `await module.evaluate()`, Node and V8
expose `sandbox.written === 2`, while QuickJS exposes `0`. This is the existing
copy-in/copy-out sandbox architecture, now extended to synchronous module
execution; solving it requires lifecycle-safe synchronization around async job
completion or a live sandbox global. The common fixture checks TLA realm
identity without claiming this stronger writeback behavior.

## Review: preserve synthetic evaluation callback writes

Review found that applying the initial in/out pair to synthetic roots overwrote
their principal-realm evaluation callback's changes to the sandbox. For
`c = vm.createContext({ value: 1 })`, a synthetic module callback assigning
`c.value = 2` left `1` on QuickJS and `2` on Node/V8. A source root importing
that synthetic module also read the stale value. The exact case was reproduced
and inspected under LLDB at `initialize_synthetic_module` before changing code.

The corrective action plan is to synchronize around source roots only, add
standalone synthetic and source-graph regressions, and preserve sandbox writes
across the explicit synthetic evaluation callback boundary. Copying in after
the callback alone can overwrite earlier source-dependency global writes, so
source graphs need their globals flushed before that callback as well.
Standalone synthetic roots must avoid that pre-callback writeback. Keep this
boundary local to module evaluation, preserve principal-realm callback behavior,
and leave asynchronous completion synchronization as the documented limitation.

Implemented the correction with small RAII-managed context stacks. Source roots
copy in/out, while synthetic roots leave the callback's sandbox changes alone.
A synthetic callback reached during synchronous source evaluation flushes
preceding source writes before crossing into the host, then copies the host's
sandbox changes in on return. Its thrown exception is saved and restored around
that synchronization. Nested synthetic callbacks in the same context skip the
pre-callback flush, since flushing there would overwrite the outer host
callback's new sandbox values. The stacks close on every exit and distinguish
real synchronous evaluation from an unrelated module waiting on top-level await.

The provider-owned regression covers successful and rejected standalone and
source-graph synthetic evaluation. The public fixture also covers a preceding
source dependency write and a nested synthetic evaluation inside the host
callback. Node and V8 pass the expanded fixture; the complete QuickJS N-API
module-state target passes 30/30. Primary and WASIX workers were notified when
the final provider patch became ready for their incremental rebuilds.

The same review pattern exposed the explicit `initializeImportMeta` host
callback: assigning `context.value = 2` there was overwritten by the source
root's final writeback. Node exposes `2`, QuickJS exposed `1`. Reproduced with
a module exporting `import.meta.x` and inspected under LLDB at
`initialize_import_meta`. The follow-up plan is to extract the already tested
module host-callback boundary into one shared helper, use it for synthetic
steps and the import-meta hook, and add public/provider regressions. This keeps
the fix inside explicit module hooks; arbitrary native callbacks retain the
provider's documented sandbox limitations.

The final helper is shared by synthetic evaluation and import-meta initialization.
The complete provider module-state target passes 31/31, including successful
and rejected import-meta initialization with prior source global writes. Node
and V8 pass the full expanded public fixture. The worker has frozen provider
edits for the final native and WASIX rebuilds.

After the final helper rebuild, QuickJS again passes the full public fixture,
including import-meta coverage, and both native `edge.napi_realms` CTests pass.
