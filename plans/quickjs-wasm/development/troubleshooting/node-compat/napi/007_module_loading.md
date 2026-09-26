# Known Issue: Module loading

| | | Remarks |
| --- | --- | --- |
| **Status** | 🟠 | QuickJS-backed `ModuleWrap` is implemented and focused native module tests pass; remaining public VM failures are Edge V8-baseline gaps and do not block QuickJS module-loading parity. |
| **Severity** | High | Package resolution and CommonJS/ESM interop decide whether real Node apps can bootstrap. |

## Current State

### Caught synchronous evaluation errors (September 2026)

Before the fix, native and WASIX QuickJS builds exit 1 after a caller catches the error
from `require()` of a throwing ESM module. The same script exits 0 with V8.
LLDB confirms that `evaluate_sync` extracts the rejected evaluation promise's
reason and throws it synchronously without marking that promise handled; V8
already marks its equivalent promise handled.

Action plan: add shared N-API coverage for synchronous exception identity and
promise rejection tracking, mark only this consumed rejection handled in the
QuickJS provider, and rerun native Edge and rebuilt QuickJS WASIX module/Pi
startup checks. Keep this fix separate from the host-JavaScript module changes.

Fixed in N-API commit `350de41625e6c24f2b79a0ae6287c8b5b889d358` by calling
`JS_PromiseMarkAsHandled` before propagating the original error. The new shared
test fails before the change and passes after it; the standard QuickJS suite
passes all 96 tests and the V8 contextify suite passes all 15. Fresh native and
WASIX Edge builds exit 0 for caught/cached module errors and the module graph
regressions, while uncaught errors and ordinary unhandled rejections still exit
1. Both start the unmodified Pi 0.87.1 CLI (`--version`). WASIX used the existing
sysroot and Wasmer 7.4.2. Full QuickJS Pi tool-loop coverage remains outstanding.

The QuickJS N-API backend should not carry a parallel C++ approximation of
Node's package resolver, CommonJS wrapper, or ESM translator policy.
The C++ CJS/module-loader hack has been removed: `unofficial_module_loader` and
`quickjs_cjs_exports` no longer exist under `napi/quickjs/src`.

The QuickJS `module_wrap`/unofficial public surface now forwards to
`napi/quickjs/src/internal/napi_module_wrap.{h,cc}`. Source-text modules,
explicit host linking, instantiate/evaluate, synthetic modules, import-meta,
dynamic import, required module facades, and synchronous `require(esm)` all use
QuickJS module records instead of the previous `napi_generic_failure` stubs.

Focused native loader and module-wrap checks pass, including
`test-internal-module-wrap`, VM synthetic modules, import-meta, dynamic import,
top-level-await, async-graph, moduleRequests, link tests, and a manual CommonJS
`require(esm)` smoke. The prior `node:test`/`Symbol.description` startup
failure was caused by missing QuickJS `Symbol.dispose` and
`Symbol.asyncDispose`; the QuickJS env initialization shim now restores those
symbols.

Two broader VM tests are explicitly excluded from the current QuickJS
module-loading gate because the rebuilt Edge V8 backend does not pass them:

- `test/parallel/test-vm-module-basic.js`: Edge V8 hangs on
  `SourceTextModule.evaluate({ timeout: 500 })`; QuickJS reaches the inspection
  assertion but its context object still exposes QuickJS-specific markers.
- `test/parallel/test-vm-module-linkmodulerequests.js`: Edge V8 passes 2/4,
  rejects `import source Foo from "foo"` as a syntax error, and reports only
  `Module is not linked` for the unlinked-child diagnostic.

These tests should be treated as diagnostics, not release gates, until Edge's V8
backend passes them or the test expectations are made Edge-specific.

## Known Incompatibility

QuickJS asks the embedder to normalize and load modules, while Node packages
expect Node's resolver, CommonJS wrapper behavior, package conditions, and
builtin module names. Framework bundles also expose pnpm symlink layouts and
mixed CJS/ESM graphs that fail if resolution is only browser-like or
filesystem-local.

Classic CommonJS loading should stay in Node's JavaScript CJS loader path under
`lib/internal/modules/cjs/loader.js`, with CJS compilation through
`internalBinding('contextify').compileFunctionForCJSLoader`. The missing parity
surface is the engine-backed `internalBinding('module_wrap')` implementation
that V8 provides and that the Node ESM loader/translators depend on.

## Design Decision

Do not resurrect the removed C++ CommonJS resolver/facade. Instead:

1. Keep `lib/internal/modules/cjs/loader.js`,
   `lib/internal/modules/esm/loader.js`, and
   `lib/internal/modules/esm/translators.js` as the source of truth for
   package resolution, package `exports`, `node:` builtins, format detection,
   CJS wrapping, JSON modules, CJS named export preparsing, and
   CJS/ESM cache interaction.
2. Implement the QuickJS backend's `unofficial_napi_module_wrap_*` APIs with a
   real QuickJS module record bridge.
3. Add only narrow QuickJS engine APIs needed to expose parsed module requests,
   link/evaluate separately, report module status/TLA, initialize
   `import.meta`, and route dynamic `import()` to the JS loader callback.
4. Keep native QuickJS loader callbacks as a prelinked-record lookup layer. They
   must map requests already resolved by Node's JS loader to QuickJS
   `JSModuleDef*`; they must not resolve packages themselves.

## Target Architecture

The implementation should add `napi/quickjs/src/internal/napi_module_wrap.h`
and `napi/quickjs/src/internal/napi_module_wrap.cc`, following the current
internal helper direction. `napi/quickjs/src/unofficial_napi.cc` should become
thin forwarding glue for the `unofficial_napi_module_wrap_*` symbols, matching
how contextify and serdes have moved into internal classes.

Each `napi_module_wrap__` record should own:

- the `napi_env`;
- the JS wrapper reference;
- the module URL/name;
- the QuickJS module value and `JSModuleDef*`;
- parsed module requests, including import attributes and phase;
- linked dependency record pointers supplied by JS `ModuleWrap.link(...)`;
- source text or synthetic export metadata as needed for diagnostics;
- status and error mirrors for the Node `ModuleWrap` status constants;
- synthetic evaluation callback and pending export values for synthetic
  modules.

The bridge should use RAII for QuickJS `JSValue` ownership and should never
store raw N-API values without a reference or a duplicated QuickJS value.

## Implementation Phases

### 1. QuickJS Module Introspection and Split Lifecycle

Add a minimal vendored QuickJS API surface in
`napi/quickjs/src/quickjs/quickjs.h` and `quickjs.c`:

- expose source module request count and request data, including import
  attributes;
- expose module status in a backend-neutral enum that can be mapped to
  `kUninstantiated`, `kInstantiating`, `kInstantiated`, `kEvaluating`,
  `kEvaluated`, and `kErrored`;
- expose `has_tla` and an async-graph query;
- expose a link-only operation around QuickJS's internal module linking;
- expose an evaluate-only operation that returns QuickJS's evaluation promise or
  namespace result as appropriate;
- expose the saved module evaluation exception.

This is the highest-risk part because QuickJS's public API currently combines
link and evaluate through `JS_EvalFunction(...)`. Node's `ModuleWrap` needs
`instantiate()` to validate and link the graph before evaluation.

### 2. Source Text `ModuleWrap`

Implement source text module creation by compiling with
`JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY`, recording the `JSModuleDef*`,
and extracting the parsed requests for `getModuleRequests()`.

`ModuleWrap.link(linkedModules)` should store JS-loader-selected dependencies
by request index. During `instantiate()` and `evaluate()`, install a QuickJS
module normalizer/loader that maps `(parent module URL, request specifier,
attributes)` to the already linked child record. This callback is a lookup
adapter only; all real resolution has already happened in JavaScript.

`getNamespace()`, `getStatus()`, `getError()`, `hasTopLevelAwait`, and
`hasAsyncGraph` should reflect QuickJS module state and Node's error
expectations closely enough for `lib/internal/modules/esm/module_job.js`.

### 3. Synthetic Modules for CJS, Builtins, JSON, and WASM Facades

Implement `unofficial_napi_module_wrap_create_synthetic(...)` using
`JS_NewCModule(...)`, `JS_AddModuleExport(...)`, and `JS_SetModuleExport(...)`.
The JS synthetic evaluation callback must run during module evaluation with
`this` bound to the JS `ModuleWrap` wrapper so existing translator code can call
`this.setExport(...)`.

This unlocks the important Node translator paths:

- CommonJS modules imported from ESM;
- CommonJS modules required from imported CJS;
- JSON modules;
- builtin ESM facades;
- CJS named export facades generated by
  `lib/internal/modules/esm/translators.js`.

### 4. `require(esm)` and Required Module Facades

Implement `evaluateSync(...)` so `require(esm)` can link and evaluate a
synchronous ESM graph and return the namespace object. If QuickJS reports TLA or
an async graph, throw the same `ERR_REQUIRE_ASYNC_MODULE` path used by
`lib/internal/modules/esm/module_job.js`.

Implement `createRequiredModuleFacade(...)` by compiling the same kind of
source-text facade V8 uses:

```js
export * from 'original';
export { default } from 'original';
export const __esModule = true;
```

The temporary facade resolver should map the synthetic `'original'` request to
the original module record. Returning a plain object is not enough for full V8
parity because the facade is expected to behave like a module namespace with
live bindings.

### 5. Dynamic `import()` and `import.meta`

Wire `setImportModuleDynamicallyCallback(...)` and
`setInitializeImportMetaObjectCallback(...)` into QuickJS instead of keeping
them as no-op stubs.

QuickJS currently handles dynamic import internally through its module loader.
For Node parity, patch or wrap that path so `import(specifier, attributes)` calls
the registered JS callback from `lib/internal/modules/esm/utils.js` and returns
its promise. The native side should preserve QuickJS's string conversion and
attribute validation, but resolution and loading must be delegated to the Node
JS loader.

For `import.meta`, call the registered initializer the first time QuickJS creates
the module's meta object, passing the meta object and the owning `ModuleWrap`.

### 6. Contextify and Format Detection Tightening

Keep CommonJS compilation in `compileFunctionForCJSLoader(...)`, but replace the
current string-search `containsModuleSyntax(...)` fallback with a parser-backed
check where possible. False positives or false negatives here can send ambiguous
`.js` entry points to the wrong loader.

This phase should stay separate from `ModuleWrap` unless tests prove it blocks
the main CJS parity path.

### 7. Verification

Use staged verification:

1. New QuickJS N-API tests for source text modules:
   - static import/export;
   - duplicate request linkage;
   - missing named export error;
   - namespace access after instantiate;
   - top-level await reporting.
2. Synthetic module tests:
   - `new ModuleWrap(url, undefined, ['default'], callback)`;
   - `setExport(...)`;
   - CJS translator facade with named exports, `default`, and
     `module.exports`.
3. Loader tests:
   - `require('./cjs.cjs')`;
   - ESM imports CJS and reads named/default exports;
   - CJS `require()` of synchronous ESM;
   - JSON import and CJS cache sharing;
   - `node:` builtin ESM facade.
4. Dynamic import and import-meta tests.
5. Existing native gates:

```sh
make build-napi-quickjs
make test-napi-quickjs
make test-napi-quickjs-only
make build-edge-quickjs-cli JOBS=4
cmake --build build-edge-quickjs-cli --target edge -j4
```

WASIX is excluded from the current native module-loading acceptance pass. When
WASIX verification is re-enabled, changes under `src/`, `lib/`, or
`napi/quickjs/` should also run:

```sh
cd /Users/syrusakbary/Development/edgejs/quickjs-wasm/ && ./build.sh
```

## Development Task Notes

Implementation planning is split under:

```text
plans/quickjs-wasm/development/dev_003_quickjs_module_loading/
```

The task notes define dependency order and write ownership for:

- QuickJS module API and core record implementation;
- Node loader interop and synthetic modules;
- dynamic import, import.meta, and required module facades;
- native tests, deferred WASIX rebuilds, and framework smoke checks.

## Non-Goals

- No C++ package resolver.
- No native CommonJS wrapper or export scanner outside Node's JS loader.
- No app-specific patches in framework output or `node_modules`.
- No broad QuickJS fork changes beyond the small APIs needed to support
  Node-compatible `ModuleWrap` behavior.
