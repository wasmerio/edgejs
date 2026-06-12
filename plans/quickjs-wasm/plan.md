# QuickJS WASM Implementation Plan

## Goal

Build a `quickjs-wasm/wasmer.toml` package that runs Edge.js as a WASIX
WebAssembly binary with QuickJS compiled into the same guest module. This should
avoid the current host-provided N-API bridge used by `EDGE_NAPI_PROVIDER=imports`
and instead provide N-API from inside the wasm binary.

The important design point is that Edge.js is already organized around an
engine boundary: `src/` uses N-API and must not depend on V8 directly. The
QuickJS path should preserve that boundary by adding a new N-API provider rather
than rewriting the runtime kernel.

## Current Status Note

The early plan below predates the later cleanup decision around module loading.
The QuickJS C++ CommonJS facade/module-loader hack has now been removed:
`unofficial_module_loader.{h,cc}` and `quickjs_cjs_exports.{h,cc}` are gone from
`napi/quickjs/src`.

The missing engine-side module primitive has since moved into vendored QuickJS
itself. Commit `577fb31caf2e973b111431b6cb009f7595cc5f7d` adds APIs for
enumerating module requests, binding host-linked modules, linking/evaluating
modules, querying module state, initializing import meta, and handling dynamic
imports. The current `napi/quickjs/src/internal/napi_module_wrap.*` code uses
those APIs to implement the V8-shaped `unofficial_napi_module_wrap_*` surface.

That does not restore the removed C++ package resolver or CommonJS export
scanner. The current design direction is to keep package resolution, CommonJS
wrapping, package conditions, and builtin policy in Node's JavaScript
loaders/translators or another proper EdgeJS-owned runtime path.

## Research Summary

- The current native path uses `EDGE_NAPI_PROVIDER=bundled-v8` and links
  `napi/v8`.
- The current WASIX path uses `EDGE_NAPI_PROVIDER=imports`, builds
  `build-wasix/edgejs.wasm`, and leaves N-API symbols unresolved for the Wasmer
  host to provide.
- `RunWithFreshEnv()` creates an environment through
  `unofficial_napi_create_env()`, then `RunScriptWithGlobals()` installs process,
  timers, module loading, primordials, builtins, and the event loop.
- Therefore the hard part is not compiling QuickJS to wasm. The hard part is
  implementing enough Node-API plus `unofficial_napi` over QuickJS for the
  existing Edge bootstrap and internal bindings.
- Existing QuickJS WASI projects are useful references, especially
  QuickJS-NG/WASI and `vercel-labs/quickjs-wasi`, but they are not drop-in
  Node-compatible runtimes.
- `quickjs-emscripten` is mostly a JS-host embedding library, not a good WASIX
  executable backend.
- Javy is useful as a QuickJS/WASI compilation reference, but it compiles
  applications to wasm rather than providing a reusable Edge.js runtime.
- StarlingMonkey is better aligned with WinterCG/Web APIs than this QuickJS
  requirement and would bypass the existing N-API architecture.

## Chosen Direction

Add a new provider:

```text
EDGE_NAPI_PROVIDER=quickjs
```

with implementation under:

```text
napi/quickjs/
```

This provider should:

- compile QuickJS or QuickJS-NG as C/C++ sources into the Edge WASIX binary;
- implement the public `node_api.h` functions used by Edge;
- implement the private `unofficial_napi.h` hooks used by Edge bootstrap,
  modules, workers, diagnostics, and testing;
- expose a normal `edge` executable target with no imported N-API symbols;
- produce `quickjs-wasm/wasmer.toml` pointing at the resulting wasm binary.

Prefer QuickJS-NG for the first prototype because it has active WASI work,
CMake-based integration, and references for reactor/event-loop APIs. Keep the
provider boundary narrow enough that switching to upstream QuickJS later remains
possible.

## Implementation Phases

### 1. Establish the Build Scaffold

1. Add `quickjs-wasm/` with:
   - `wasmer.toml`;
   - a build script, likely `quickjs-wasm/build.sh`;
   - a short README with the supported smoke commands.
2. Mirror the existing WASIX build flow from `wasix/build-wasix.sh`, but use a
   separate build directory such as `build-quickjs-wasix`.
3. Run the build with `wasixcc` available from:

   ```sh
   nix run github:wasix-org/wasinix#wasixcc
   ```

   or an equivalent shell where `wasixcc`, `wasixcc++`, `wasixar`, and
   `wasixranlib` are on `PATH`.
4. Extend `CMakeLists.txt`:
   - allow `EDGE_NAPI_PROVIDER=quickjs`;
   - add `napi/quickjs` as a subdirectory for that provider;
   - link `edge_node_api` and `edge_runtime` against `napi_quickjs`;
   - make the same lifecycle hook paths currently gated by
     `EDGE_BUNDLED_NAPI_V8` available to QuickJS, preferably through a neutral
     compile definition such as `EDGE_EMBEDDED_NAPI_PROVIDER`;
   - do not enable `EDGE_ALLOW_UNDEFINED_IMPORTS` for this provider except for
     unrelated WASIX system imports.
5. Ensure `napi/quickjs` CMake source lists only checked-in files before the
   first configure/build attempt.
6. Add a link check that fails if the final QuickJS wasm still imports N-API
   symbols.

Deliverable: a wasm binary that links QuickJS into the guest and starts, even if
it only prints a provider initialization error.

### 2. Build a Minimal QuickJS Provider

1. Add `napi/quickjs` with:
   - `CMakeLists.txt`;
   - a CMake `FetchContent` import for QuickJS-NG from a pinned release
     archive with `URL_HASH SHA256`;
   - `napi_quickjs_env.*`;
   - `napi_quickjs_values.*`;
   - `napi_quickjs_unofficial.*`.
2. Implement environment creation and teardown:
   - `unofficial_napi_create_env`;
   - `unofficial_napi_create_env_with_options`;
   - `unofficial_napi_release_env`;
   - `unofficial_napi_release_env_with_loop`;
   - cleanup and destroy callbacks.
3. Map `napi_env`, `napi_value`, and `napi_ref` to QuickJS runtime/context,
   `JSValue`, and explicit reference records.
   - Placeholder C++ value graphs are acceptable only for link bring-up; replace
     them with QuickJS `JSValue` ownership before relying on GC or runtime
     correctness.
4. Implement the primitive value APIs needed by bootstrap:
   - undefined/null/boolean/number/string creation;
   - type checks and conversions;
   - object creation;
   - property get/set/has/delete;
   - function creation and calls;
   - error creation and pending exception state.
5. Add a tiny provider-only C++ test that creates an env, evaluates
   `1 + 1`, and tears down without Edge bootstrap.

Deliverable: the provider can create a QuickJS context and evaluate simple JS
through N-API-like calls.

### 3. Run the Smallest Edge Bootstrap

1. Start with `EdgeRunScriptSource()` rather than full CLI script loading.
2. Support enough N-API for:
   - `console`;
   - `process` object installation;
   - `internalBinding`;
   - builtin execution through `EdgeExecuteBuiltin`;
   - basic script execution without reintroducing the removed C++ CJS loader
     hack.
3. Implement `unofficial_napi_process_microtasks` with QuickJS job draining.
4. Stub non-critical V8-specific diagnostics with conservative values where
   Edge can continue:
   - heap statistics;
   - CPU/heap profiler hooks;
   - V8 version-like metadata.
5. Do not implement workers, ESM, snapshots, inspector, or profiling in this
   phase unless bootstrap requires a narrow stub.

Deliverable:

```sh
edge -e "console.log('hello from quickjs')"
```

runs inside the QuickJS-backed WASIX binary.

### 4. Expand N-API by Runtime Surface

Implement APIs in the order Edge is likely to hit them:

1. Core values:
   - arrays;
   - symbols;
   - property descriptors;
   - strict equality;
   - instance checks;
   - private data / wrap / unwrap.
2. Functions and callbacks:
   - callback info;
   - `this`;
   - constructor calls;
   - thrown exceptions;
   - fatal error hooks.
3. References and lifecycle:
   - strong and weak references;
   - finalizers;
   - env cleanup hooks;
   - external values.
4. Buffers and binary data:
   - ArrayBuffer;
   - TypedArray;
   - DataView;
   - Buffer compatibility;
   - external backing-store ownership rules.
5. Promises and async:
   - promise creation;
   - promise details;
   - handled markers;
   - async work hooks needed by timers and libuv.
6. Module support:
   - no C++ CommonJS facade/module-loader hack in the QuickJS backend;
   - ESM/source text module support once enough `ModuleWrap` behavior exists;
   - Node-compatible CommonJS/ESM behavior only through Node's JavaScript
     loaders/translators or another proper EdgeJS-owned runtime layer;
   - synthetic modules and cached data can initially be unsupported unless tests
     or bootstrap require them.

Use compile errors, missing-symbol reports, and failing smoke tests to maintain a
tracked N-API gap list. Avoid implementing broad APIs before a caller needs
them.

Deliverable: a checked-in compatibility matrix for implemented, stubbed, and
unsupported N-API / `unofficial_napi` calls.

### 5. Package as `quickjs-wasm`

1. Create `quickjs-wasm/wasmer.toml` similar to the root `wasmer.toml`, but point
   at:

   ```text
   ../build-quickjs-wasix/edgejs.wasm
   ```

   or copy the artifact into `quickjs-wasm/edgejs.wasm` during packaging.
2. Mount certificates as the current package does:

   ```toml
   [fs]
   "/usr/local/ssl" = "../ssl-certs"
   ```

3. Decide whether JS builtins are:
   - compiled into `builtin_catalog.cc` only; or
   - mounted from `lib/` during development.

   Prefer compiled builtins for distribution and optional mounts for debugging.
4. Add package smoke commands:

   ```sh
   wasmer run quickjs-wasm/wasmer.toml -- -e "console.log(1 + 1)"
   wasmer run quickjs-wasm/wasmer.toml -- examples/hello.js
   ```

Deliverable: `quickjs-wasm/wasmer.toml` can run the QuickJS-backed Edge binary
through Wasmer.

### 6. Bring Up Runtime Features Incrementally

After basic CLI execution works, add features in this order:

1. Filesystem-backed `require()` and path resolution.
2. Timers and event-loop checkpoints.
3. `Buffer` and encoding paths.
4. `fs`, `path`, `os`, and process APIs.
5. HTTP parser and minimal server/client networking.
6. Crypto and TLS.
7. Workers.
8. ESM and top-level await.
9. Diagnostics, profiling, heap snapshots, and inspector-like APIs.

Each feature should land with one end-to-end WASIX smoke test and the smallest
provider API expansion needed to support it.

### 7. Optimize Only After Correctness

Do not start with snapshots or bytecode caching. First make source execution
correct and observable.

Then evaluate:

- QuickJS bytecode for builtins;
- precompiled builtin catalog generation;
- startup snapshots based on QuickJS-NG/WASI references;
- binary size reductions;
- `wasm-opt --emit-exnref` and the same exception mode used by the current
  WASIX build.

Snapshots are a later optimization because they interact with native callbacks,
libuv state, file descriptors, promises, and embedder data.

June 9, 2026 native QuickJS CLI measurement for the Astro apps-dashboard server:
temporary probes around QuickJS CJS and ESM compile calls showed about 124 ms of
parse/compile work before the HTTP listener, and about 144 ms more on the first
`/signin` render. Across startup plus first real request, the upper-bound saving
from direct bytecode for all loaded app, package, and builtin JS was about
268 ms of parser/compiler time over 8.15 MB of source. App/package files
accounted for about 184 ms of that total; Node builtins accounted for about
86 ms. The largest lazy hotspot was `lucide-react`, which was parsed once for
CommonJS/module-syntax detection and again for execution, so a bytecode plan
should also include module-format metadata or detection caching.

## Testing Strategy

Use a staged gate:

1. Provider unit tests:
   - values;
   - functions;
   - exceptions;
   - references;
   - ArrayBuffer/Buffer;
   - promises and microtasks.
2. Native Edge runner tests that already call `EdgeRunScriptSource()`.
3. WASIX smoke tests using the QuickJS package.
4. Selected Node compatibility tests for the feature being brought up.
5. Benchmark only after the API surface is stable enough to compare.

Useful early smoke cases:

```js
console.log("ok")
console.log(process.argv.length)
setTimeout(() => console.log("timer"), 0)
const fs = require("fs"); console.log(typeof fs.readFileSync)
```

## Main Risks

- N-API compatibility depth is the dominant risk.
- QuickJS has different GC, job queue, stack, exception, and module semantics
  from V8.
- Some current `unofficial_napi` APIs are V8-shaped and should be stubbed,
  redesigned, or isolated for QuickJS instead of copied literally.
- Workers and structured clone are likely to be expensive because they require
  cross-context value transfer semantics.
- Full ESM support requires a deeper QuickJS module-loader integration than
  simple CommonJS execution. QuickJS itself does not lack CommonJS more than V8
  does; Node implements CJS/ESM around the engine. The QuickJS risk is
  implementing enough `ModuleWrap`, package resolution, dynamic import, and
  CJS facade behavior so Node's JS loaders can drive the same semantics.
- WASIX threading, atomics, and exceptions must stay aligned with the existing
  `wasix/build-wasix.sh` flags.

## Success Criteria

The first complete version is successful when:

- `quickjs-wasm/wasmer.toml` exists and points at a QuickJS-backed wasm binary;
- the binary has no imported N-API symbols;
- `edgejs -e "console.log('hello')"` works through Wasmer;
- simple file execution works;
- the implemented N-API surface is documented;
- unsupported runtime features fail clearly rather than crashing.
