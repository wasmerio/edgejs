# Fork delta audit

Status: audit complete; integration verification is owned by the coordinator.
Owner: audit worker; write ownership: this note only. No engine edits or commits.
Dependencies: fetched refs and coordinator's resolved engine working tree.

## Comparison boundary

Compared Wasmer `origin/master` at `7b8ab77` with QuickJS-NG
`upstream/master` at `6d46d07` (0.17.0). Their merge base is
`fd0a0210b7be00957751871e7e01b8291268fc29`. There are 129 upstream commits
missing from the fork and 21 non-merge fork-only commits. Existing fork commit
history should remain; superseded implementations can be removed in the merge
resolution while retaining their provenance.

Read the development index and existing module-loading notes before reviewing.
The existing source-phase limitation is documented in
`../dev_003_quickjs_module_loading/003_dynamic_import_import_meta_required_facade.md`:
the fork exposes parsing/introspection metadata, but does not implement source
module value semantics completely.

## Fork commit inventory

| Fork commit | Decision | Evidence / required adaptation |
| --- | --- | --- |
| `577fb31` module host APIs | Retain, simplify overlap | Upstream `8ef0e71` now handles request identity and module cache identity by specifier plus attributes. Use upstream attribute comparison/parser ownership, adding the fork's phase to request deduplication. Keep host link/evaluate/status/request APIs and import-meta/dynamic-import callbacks; upstream does not expose their equivalents. |
| `41b00d4` CallSite extensions | Retain extras, merge overlap | Upstream `a3f1b38` supplies constructor frame tracking / `isConstructor`; `3d3cf3d` returns null for absent native positions. Keep additional Node-facing methods, sourceURL extraction, and `JS_GetCurrentStackTrace`. Preserve upstream `c846cb1` ownership fix. |
| `d0a8c1e` native weak refs | Retain links; remove obsolete buffer workaround | Native weak links still support `napi_ref__`. Convert `JS_GetRefCount` to upstream arena-aware `JS_REF_COUNT`. Upstream `4369dd6` fixes detached ArrayBuffer backing-store double free, allowing removal of the fork-only `JS_GetArrayBufferFreeInfo` API and N-API workaround together. |
| `1551fac` function prototype source-position removal | Retain | Still an intentional V8 compatibility difference. Upstream continues to expose these properties. Tests must assert the intended fork behavior. |
| `e12dbbd` error messages / typed-array length conversion | Retain | No upstream equivalent for these Node-facing messages. |
| `042e123` V8 strings/messages | Retain | Includes replacement-character behavior for isolated UTF-16 surrogates when producing UTF-8, plus Node error wording; upstream differences remain. |
| `1e27b5e` promise before/after callbacks | Retain | Upstream has the public promise hook API and INIT/RESOLVE paths, but still lacks this reaction-job instrumentation. Restore helper type to `JSAsyncFunctionData` after taking upstream coroutine implementation. |
| `a091f0e` lazy stacks / private symbols | Split | Retain/adapt lazy formatting. Replace custom `JS_NewPrivateSymbol` with upstream `7e322b3`, which supports null descriptions and frees the temporary atom (the fork version leaked it). |
| `24df0a3` preview entries | Retain | `JS_PreviewEntries` remains fork-only and is consumed by N-API inspection. |
| `2a875f2` promise reaction handler fallback | Retain | Needed to identify the promise for await reaction handlers when the other resolving-function slots are undefined. |
| `5f23ab0` WASIX atomics/thread checks | Retain | Upstream still excludes all `__wasi__` from atomics/pthreads. Preserve the narrower `__wasm_atomics__` opt-in in `quickjs.c` and `cutils.h`. |
| `cec4427` lazy stack setter | Retain, fix installation success test | Upstream Error.prototype setter is separate from this own accessor. Keep assignment before first stack access; correct `JS_DefinePropertyGetSet` success comparison described below. |
| `9a59f17` WASI stack guard | Remove implementation delta | Upstream `a2563ad` now enables configurable WASI stack protection and additionally prevents subtraction underflow when stack size exceeds stack top. |
| `a5522a7` coroutine GC rewrite | Replace implementation with upstream | Upstream `7955cfd` fixes the same missing closure-to-coroutine reachability edge using a different object model; do not combine them. |
| `2e7bcf4` coroutine GC comment | Superseded | Describes the replaced implementation. |
| `14bf204` async-frame teardown guard | Superseded with GC rewrite | Specific to the fork's separately allocated `JSAsyncFunctionState`. Preserve original failure workloads as regression coverage. |
| `2d9d094` native function toString formatting | Retain | Upstream still emits its multiline formatting. |
| `cfcbf33` CI trigger commit | No source delta | Historical empty trigger commit. |
| `3b563d4` UBSAN fixes | Split | Generator nullable-state fix is obsolete with upstream embedded state. Retain unresolved-module null check, which supports fork deferred linking. Source-position test changes remain consistent with fork behavior. |
| `c723e29` CI trimming | Retain fork scope | Upstream publishing/docs release jobs are not appropriate for this fork. Keep sanitizer and supported native lanes and run new upstream regression cases through a compatible driver. |
| `9d5513a` upstream api-test/test262 omissions | Reassess tests selectively | Several upstream expectations differ intentionally (deferred modules, lazy formatting, function source properties, error text). Do not describe omitted tests as passed or mask genuine new failures with blanket skips. |

## Coroutine GC: choose one complete implementation

The fork's `a5522a7` makes `JSAsyncFunctionState` itself a counted GC object,
removes `JSAsyncFunctionData`, and makes all open variable references GC objects.
Its follow-up `14bf204` neutralizes outstanding frame references during teardown.

Upstream `7955cfd` instead retains `JSAsyncFunctionData` and embedded generator
states. It adds `JSStackFrame.cur_gc_obj` and snapshots `JSVarRef.is_coro` when
the reference is created. Only detached references and open coroutine references
are marked as GC objects. The snapshot matters because generator prologue
references can be created before their owner is installed. It also handles
reentrant `close_var_ref` and closes the owned reference before releasing its
coroutine count.

The merge must take upstream's definitions, marking, close/free paths, and the
whole coroutine state machine together. The fork's promise-reaction helper is
the only retained adaptation requiring the old upstream wrapper type. Combining
unconditional fork marking with upstream non-coroutine references would corrupt
the GC lists. Coordinator restored these regions wholesale; review of the
resolved diff found no remaining mixed state representation.

Preserve and execute the upstream tests:

- `tests/suspended-coroutine-closure-gc.js`
- `tests/suspended-coroutine-mapped-arguments-gc.js`
- `tests/suspended-generator-closure-gc.js`

Also retain the original EdgeJS stream iterator/async-generator workload that
motivated the fork fix. The three upstream tests alone do not establish that
every original application failure is covered.

## Error.stack and CallSite integration

Upstream `9b57175` implements the Error.prototype.stack accessor proposal. It
adds object-data marking/finalization to actual Error objects and stores ordinary
stack strings in `u.object_data`. Genuine Errors consequently need not have an
own `stack` property. This does not replace lazy `Error.prepareStackTrace`:
upstream still calls prepare during backtrace construction.

The resolved merge retains own lazy accessors when a prepare callback exists,
including `can_store_error_stack(error_obj)` in their eligibility check, while
unprepared errors use the upstream slot. DOMException and capture targets keep
the upstream own-property paths. Constructor CallSite naming is unified through
the retained fork `is_constructor` member, with no duplicate field; upstream
native-position null semantics remain.

Review uncovered an inherited installation bug: `JS_DefinePropertyGetSet`
returns 1 on success, but the lazy path tested `== 0`. Before correction, the
resolved Debug engine printed `1 2` for this direct probe (one callback during
construction and a second callback on first access):

```js
let calls = 0;
Error.prepareStackTrace = (_, frames) => (calls++, frames);
const error = new Error();
const before = calls;
const stack = error.stack;
print(before, calls);
```

`Error.captureStackTrace` eagerly formatted and replaced its successfully
installed accessor with a data property. Coordinator corrected the success test
to `> 0` and added a lazy-stack regression. New upstream
`tests/callsite-isconstructor.js` assumes eager callback execution; change the
test to read the created error's `.stack` explicitly while preserving its
constructor/nonconstructor assertions.

The rebuilt merge was rerun against the direct probe and printed `0 1`,
confirming deferred, single-call formatting.

Upstream `c846cb1` must remain intact: insertion via
`JS_DefinePropertyValueUint32` consumes its value even on failure, and
`js_new_callsite` clears source `JSValue` fields once ownership transfers.
Both behaviors are present in the reviewed merge. These matter under OOM,
including when lazy getter creation allocates after the CallSite array exists.

## Modules, bytecode, and arena allocation

- Upstream `8ef0e71` replaces the fork's handwritten attribute equality helper
  and supplies a cache key that includes attributes. Keep its parser output
  ownership contract: callers free the partial attributes object on parse
  failure. The resolved `js_parse_import` and `js_parse_from_clause` do this.
- Host-provided child modules, deferred compile-only resolution, and exposed
  lifecycle callbacks remain necessary for N-API ModuleWrap. The retained
  recursive resolution logic resets the resolved flag on failure and checks
  that each requested child was supplied.
- Name-only referrer/import-meta lookup remains an upstream limitation when
  multiple module records have the same filename but different attributes.
  Do not claim complete identity-aware referrer support from this merge.
- The binary format moves from `BC_VERSION 26` to `28`; cached bytecode must be
  regenerated/rejected through existing cache fallback. N-API worker confirmed
  version exceptions are cleared and reported as rejected; compile-on-reject
  falls back. Existing cache round-trip, mismatched-source, and empty-cache
  tests passed Release and Debug.
- `JS_WriteModule` still serializes request names, not attributes or source
  phases; `JS_ReadModule` resolves dependencies eagerly and reconstructs empty
  attributes/evaluation phase. This limitation predates the merge. N-API's
  bytecode stub module loader already addresses eager resolution. Expanding the
  engine wire format would be separate work requiring explicit compatibility
  decisions.
- Upstream `9de2921` moves refcounts into allocator headers. The resolved
  `JS_GetRefCount` uses `JS_REF_COUNT(JS_VALUE_GET_PTR(v))`, not the obsolete
  public/body-header cast. Retained weak-link allocations use the engine
  allocator consistently. No stale `JSRefCountHeader` casts remain in the
  reviewed engine.
- Upstream removes `JSClass` from the public header (`5e8c99e`) and removes
  `js_realloc2` (`b16e7bd`). N-API compilation must use supported APIs rather
  than restoring those internal layouts.

## WASIX-specific checks

Retain Wasmer atomics/thread opt-in and take upstream WASI stack guard exactly.
Upstream `0fdea21` additionally requires WASI `malloc_usable_size` in
`cutils.h`; an actual WASIX compile is needed to verify the deployed sysroot
provides that symbol. Native success does not cover this requirement.

## Audit verification and limits

Read-only review used refs for the original comparison, then the coordinator's
resolved working diff for silent conflicts. No source or workflow edits were
made by this worker.

The built Debug engine passed an additional async-generator probe using global
`-e` with dynamic `import("qjs:std")`: a closure retains a suspended generator's
local through GC and return; 500 abandoned generator cycles are then collected
with forced GC every ten iterations. This is smoke evidence, not sanitizer or
full application validation.

The initial probe using `qjs --std` exited 139 before executing user code. LLDB
stopped in `js_inner_module_linking` with a null module at the status access.
Even `qjs --std -e 'print("hello")'` has the same failure because `--std`
internally imports modules; this was not evidence of coroutine failure.
An independent baseline build from `git archive origin/master` at `7b8ab77`,
under `/private/tmp/quickjs-fork-audit-baseline`, reproduced exit 139 for the same
command. Baseline LLDB backtrace:

```text
js_inner_module_linking(m=NULL) quickjs.c:31023
js_inner_module_linking          quickjs.c:31041
js_link_module                  quickjs.c:31217
JS_EvalFunctionInternal          quickjs.c:37830
JS_EvalFunction                  quickjs.c:37846
eval_buf                        qjs.c:128
main                            qjs.c:645
```

The static module/`--std` driver failure is therefore confirmed pre-existing.
Module regressions should run through the resolving `run-test262` driver or
the N-API/Edge host unless the standalone qjs driver is updated intentionally.

Full engine/N-API/EdgeJS builds, sanitizer runs, WASIX checks, PR creation, and
CI statuses are owned by their corresponding workers/coordinator and are not
claimed by this audit note.
