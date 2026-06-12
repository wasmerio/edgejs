# Plan: Build Modularization and First-Class Internal Binding Registry

Status: Implementation complete with imports-provider configure/build verified

Scope: `src/`, root `CMakeLists.txt`, `cmake/`, `tests/runners/`,
`tests/CMakeLists.txt`, and architecture documentation.

Canonical development index entry:
`plans/quickjs-wasm/development/dev_005_build_registry_modularization/`.

## Goal

Make EdgeJS easier to extend by removing the duplicated internal-binding
dispatch path and modularizing the root build. The useful lesson from
`hermes-node` is structural: a small registry, explicit manifests, subsystem
targets, and a root build that reads like a table of contents.

This plan is intentionally narrower than a full runtime cleanup. The highest
value work is:

- one binding registry, one binding manifest, one cache owner;
- no binding-dispatch logic in the module loader;
- root CMake split into provider/dependency/subsystem helpers;
- enough source organization that adding a binding is obvious.

## Non-Goals

- Do not change vendored Node JavaScript under `lib/`.
- Do not change N-API provider boundaries or move QuickJS-only behavior into
  shared runtime files.
- Do not redesign package resolution, CommonJS, ESM, or module-format policy.
- Do not fully split `edge_module_loader.cc` beyond removing binding-registry
  responsibilities.
- Do not move every `src/edge_*.cc` file as part of the first implementation.
  Wider source-tree cleanup can follow once the registry and build graph are
  stable.

## Current State

### Binding Dispatch

Today `internalBinding("name")` is routed through multiple layers:

```text
JS internalBinding(name)
  -> NativeGetInternalBindingCallback in edge_module_loader.cc
  -> internal_binding::Resolve(...) in internal_binding/dispatch.cc
  -> binding-specific Resolve* trampoline or implementation
  -> ResolveCallbacks back into edge_module_loader.cc
  -> DispatchResolveBinding(...) string chain
  -> GetOrCreateBinding(...)
  -> actual EdgeInstall*Binding implementation
```

The practical problems:

- there are two binding dispatch tables;
- many `src/internal_binding/binding_*.cc` files are thin trampolines;
- `ResolveCallbacks` exists mainly to work around the dependency cycle between
  `internal_binding/` and the module loader;
- binding caches live in `ModuleLoaderState`, so the binding surface cannot be
  tested without standing up the loader;
- some binding builders live inside `edge_module_loader.cc`, increasing churn in
  the loader whenever binding behavior changes.

### Root Build

The root `CMakeLists.txt` currently owns too many concerns at once:

- project options and provider selection;
- vendored dependency setup;
- OpenSSL and ICU target setup;
- generated builtin catalog rules;
- the large `edge_runtime` source list;
- CLI executables;
- tests.

This makes source movement risky and turns small binding additions into edits in
several central files.

## Target State

### 1. First-Class Binding Registry

Add a small registry module:

```text
src/binding_registry/
  binding_registry.h
  binding_registry.cc
  binding_list.h
```

`binding_list.h` is the single manifest for compiled-in bindings:

```cpp
// One line per binding, sorted by JS-visible name.
EDGE_BINDING(async_wrap, InitAsyncWrap)
EDGE_BINDING(buffer, InitBuffer)
EDGE_BINDING(cares_wrap, InitCaresWrap)
EDGE_BINDING(contextify, InitContextify)
EDGE_BINDING(fs, InitFs)
EDGE_BINDING(module_wrap, InitModuleWrap)
EDGE_BINDING(tcp_wrap, InitTcpWrap)
EDGE_BINDING(zlib, InitZlib)
```

Use an explicit manifest rather than static self-registration. It is easier to
review, safe under static linking and WASIX, and doubles as documentation of the
native compatibility surface.

Preferred API shape:

```cpp
namespace edge::binding_registry {

using BindingInit = napi_value (*)(napi_env env);

napi_value Get(napi_env env, std::string_view name);
bool Has(std::string_view name);
std::vector<std::string_view> Names();
void FinalizeEnv(napi_env env);

}  // namespace edge::binding_registry
```

Registry responsibilities:

- lookup binding entries by name;
- lazily install exports on first request;
- cache one exports object per binding name per `napi_env`;
- own cache cleanup during environment teardown;
- preserve unknown-binding behavior;
- expose the binding name list for diagnostics and tests.

Per-env storage and thread safety: the registry state lives in a new
environment slot (`kEdgeEnvironmentSlotBindingRegistryState`, alongside the
existing slots in `edge_environment.h`), not in a global `env -> cache` map.
Worker environments run on their own threads; a per-env slot needs no locking,
a global map would. The static manifest table itself is read-only after
startup.

Teardown contract: `FinalizeEnv` must release cached refs **and** set a
per-env finalized flag so that later `Get()` calls return null instead of
re-installing bindings during teardown. This mirrors the `state->finalized`
guard in today's `NativeGetInternalBindingCallback`.

Unknown-name contract (verified against current behavior, and deliberately
different from Node, which throws): unknown names return `undefined` without
throwing; if an exception is already pending, return `nullptr`. Vendored
`lib/` code may feature-detect with try/catch, so do not "fix" this toward
Node semantics as part of this plan — the Phase 0 fixture records behavior,
not just names.

The module loader should only parse the JS argument and call
`binding_registry::Get(env, name)`. Preserve the existing
`EDGE_TRACE_INTERNAL_BINDING` stderr tracing when the callback body moves
into the registry.

### 2. Binding Initializer Convention

Move toward one initializer shape:

```cpp
napi_value InitFoo(napi_env env);
```

The initializer creates and returns the exports object. It should not know
whether it was called by JS `internalBinding()`, another native binding, or a
test. If a binding needs another binding, it calls
`binding_registry::Get(env, "other")` directly.

Special cases stay explicit:

- aliases such as `util` / `types`;
- shared sub-objects such as `os` / `os_constants`;
- stable stubs such as `inspector` or `profiler`;
- provider-sensitive fallbacks.

These should be visible in the manifest or in named adapter functions, not
hidden inside loader dispatch code.

### 3. Build Organization

Split the root build mechanically before broad source movement:

```text
CMakeLists.txt
cmake/
  EdgeOptions.cmake
  EdgeNapiProvider.cmake
  EdgeVendoredDeps.cmake
  EdgeOpenSSL.cmake
  EdgeICU.cmake
  EdgeBuiltinCatalog.cmake
src/CMakeLists.txt
src/binding_registry/CMakeLists.txt
src/bindings/CMakeLists.txt
src/crypto/CMakeLists.txt
```

The first pass can keep existing file locations and only move source ownership
into subsystem targets. Source-tree moves should happen after dispatch is
collapsed, when there are fewer files to move.

Essential targets:

```text
edge_runtime          existing public/aggregate target
edge_runtime_core     runtime/environment/loader aggregation
edge_loader           module loader and builtin catalog
edge_binding_registry registry only
edge_bindings         internal binding implementations
edge_crypto           existing crypto implementation target
edge_cli              CLI library/executable support
```

Rules to enforce:

- `edge_binding_registry` must not depend on `edge_loader`;
- `edge_bindings` should not depend on `edge_loader`;
- provider selection and provider compile definitions live in one CMake helper;
- generated builtin catalog rules live in one CMake helper;
- the existing `edge_runtime` target name remains usable by tests and external
  build references during migration.

### 4. Minimal Source Organization

Create only the directories needed to make the registry/build cleanup pay off:

```text
src/binding_registry/
src/bindings/
```

Move or merge binding files only when it reduces dispatch complexity:

- delete trampoline `binding_*.cc` files after their installers are in
  `binding_list.h`;
- keep large implementation pairs side by side rather than forcing giant merges;
- move loader-resident binding builders out of `edge_module_loader.cc` only when
  doing so removes registry responsibilities from the loader.

Defer a full `src/cli`, `src/runtime`, and `src/loader` source-tree migration
until this work is green.

## Implementation Phases

Implementation status in this branch:

- added `src/binding_registry/{binding_registry.h,binding_registry.cc,binding_list.h}`;
- made `binding_list.h` the single manifest for all 63 JS-visible binding names;
- routed `NativeGetInternalBindingCallback` directly through
  `edge::binding_registry::Get(env, name)` with no fallback resolver;
- deleted `internal_binding/dispatch.{h,cc}`, `ResolveCallbacks`, the duplicate
  loader dispatch chain, and `ModuleLoaderState::binding_cache`;
- converted remaining substantive `src/internal_binding/binding_*.cc` files from
  `Resolve*(env, ResolveOptions)` entry points to `Init*(env)` initializers;
- deleted thin trampoline `binding_*.cc` files whose installers are now manifest
  entries;
- replaced native callback-based binding fetches in async/handle/stream/TLS,
  process, messaging, and the util-symbol runtime path with
  `binding_registry::Get`;
- kept loader-owned builders (`builtins`, `contextify`, `modules`, `options`,
  `trace_events`, and `uv`) inside `edge_module_loader.cc`, but exposed them as
  named installer functions via `edge_loader_bindings.h` so the registry remains
  the only JS-name resolver;
- added `edge_binding_registry`, `edge_loader`, `edge_runtime_core`,
  `edge_bindings`, and `edge_crypto` subsystem object targets, with source-list
  ownership under `src/`;
- extracted options and target-environment detection, provider normalization,
  vendored dependency setup, OpenSSL target setup, ICU target setup, and builtin
  catalog generation into `cmake/Edge*.cmake` helpers;
- added `edge_test_6_binding_registry_phase04` coverage for manifest sortedness,
  cache identity, unknown-name behavior, finalization tombstoning, and JS
  `internalBinding()` cache behavior;
- tightened the internal-binding dispatch guard so the loader callback cannot
  pass a fallback resolver; and
- updated `ARCHITECTURE.md` with the target graph, CMake ownership map, registry
  responsibilities, and "Adding A Binding" workflow.

Verification note: direct syntax checks, source guards, and the CTest
internal-binding dispatch guard pass. A fresh no-network configure/build also
passed with:

```sh
cmake -S . -B /private/tmp/edgejs-cmake-imports-check \
  -DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY \
  -DEDGE_NAPI_PROVIDER=imports \
  -DEDGE_BUILD_CLI=OFF \
  -DEDGE_BUILD_NAPI_TESTS=OFF
cmake --build /private/tmp/edgejs-cmake-imports-check --target edge_runtime -j4
```

That verified the extracted CMake helpers, generated builtin catalog rule,
subsystem object targets, and aggregate `edge_runtime` source ownership. The
remaining V8, QuickJS, and WASIX runtime lanes still need their provider-specific
build/test commands before this can be considered fully release-gated.

### Phase 0: Baseline and Guardrails

- Record current dirty worktree state.
- Record green or known-failing baselines for V8, QuickJS, and WASIX lanes.
- Snapshot the runtime `internalBinding` name surface into a fixture,
  including behavior for unknown names (`undefined`, no throw), not just the
  name list.
- Statically enumerate BOTH dispatch tables and diff them: the
  `dispatch.cc` `kResolvers` table and the `DispatchResolveBinding` chain in
  `edge_module_loader.cc` do not have the same name set (example:
  `os_constants` is native-reachable only). For each native-only name, decide
  whether it becomes a manifest entry or stays a private helper — otherwise
  Phase 2 silently drops names.
- Add an empty `test_6_binding_registry.cc` scaffold, wired into the build but
  not yet asserting registry behavior. Verify CI builds with
  `EDGE_BUILD_NAPI_TESTS=ON` and runs the `tests/runners` binaries; if it does
  not, add that to the CI lanes so registry tests are enforced, not
  local-only.
- Record the current binding cache and environment teardown path.

Gate:

```sh
git status --short
make build-edge-quickjs-cli JOBS=4
make test-quickjs-only TEST_JOBS=4
make build JOBS=4
make test-only TEST_JOBS=4
```

If a full baseline is too expensive, record exactly which gates were deferred.

### Phase 1: Registry Core, Not Wired Into Runtime

- Add `src/binding_registry/{binding_registry.h,binding_registry.cc,binding_list.h}`.
- Populate `binding_list.h` with adapter entries that call existing installers.
- Implement sorted lookup, per-env cache (env-slot-backed, per §1), `Names()`,
  `Has()`, and `FinalizeEnv`.
- Add registry unit tests that do not require the JS module loader.
- Keep `NativeGetInternalBindingCallback` on the old path until registry tests
  are green.

Tests:

- known name installs once and returns the same object;
- unknown name returns `undefined` without throwing (the documented contract);
- `Names()` matches the manifest;
- `FinalizeEnv` releases cached refs cleanly and `Get()` after finalize
  returns null rather than re-installing;
- manifest sortedness is asserted.

Gate:

```sh
make build-edge-quickjs-cli JOBS=4
cmake -S . -B build-edge-quickjs-cli -DEDGE_BUILD_NAPI_TESTS=ON
cmake --build build-edge-quickjs-cli --target test_6_binding_registry -j4
# run the tests/runners binary for the registry phase
./build-edge-quickjs-cli/tests/test_6_binding_registry
```

(Adjust target/binary names to whatever `tests/CMakeLists.txt` produces — the
point is the gate must *run* the registry tests, not just compile the
runtime.)

### Phase 2: Wire Registry and Collapse Double Dispatch

- Point `NativeGetInternalBindingCallback` at `binding_registry::Get`.
- Route the second JS-facing accessor through the registry as well:
  `EdgeGetBuiltinInternalBinding` (the "internalBinding loader" object,
  consumed by `edge_runtime.cc`) must resolve through the same path as
  `internalBinding()`, or be explicitly documented as loader-owned.
- Move binding cache ownership out of `ModuleLoaderState`.
- Replace trampoline bindings with direct manifest entries.
- Convert substantive bindings from `Resolve*(env, ResolveOptions)` toward
  `Init*(env)`.
- Replace native use of callback-based binding resolution with direct
  `binding_registry::Get` (callers of `EdgeGetInternalBinding` in
  edge_tls_wrap, edge_process, edge_async_wrap, edge_handle_wrap,
  edge_stream_base, binding_messaging, edge_runtime).
- Extract loader-resident binding builders only when needed to remove dispatch
  and cache ownership from `edge_module_loader.cc`.
- Delete obsolete dispatch code after all entries resolve through the registry.

Recommended migration slices:

- stateless/simple bindings;
- process, os, util, errors, options;
- buffer, url, string/encoding helpers;
- loader/VM-sensitive bindings such as `contextify` and `module_wrap`;
- stream/net/fs bindings;
- crypto, TLS, HTTP, HTTP/2, DNS, compression;
- stubs and provider-sensitive bindings.

Gate after each slice:

```sh
make build-edge-quickjs-cli JOBS=4
./build-edge-quickjs-cli/edge -e "const a=internalBinding('buffer'); const b=internalBinding('buffer'); console.log(a === b)"
```

Gate after the full phase:

```sh
make test-quickjs-only TEST_JOBS=4
make build JOBS=4
make test-only TEST_JOBS=4
```

### Phase 3: Mechanical CMake Modularization

- Extract provider logic into `cmake/EdgeNapiProvider.cmake`.
- Extract vendored dependency setup into CMake helper modules.
- Extract OpenSSL, ICU, and builtin catalog generation into dedicated helpers.
- Add `src/CMakeLists.txt` and subsystem CMake files.
- Convert `edge_runtime` into an aggregate target that links subsystem targets.
- Keep target names stable for tests and external references.
- Move llhttp sources out of the runtime source list into a proper dependency
  target if it can be done mechanically.

Gate:

```sh
cmake --build build-edge-quickjs-cli --target edge -j4
make build-edge-quickjs-cli JOBS=4
make build JOBS=4
```

Run clean configure/builds for provider and platform lanes before considering
the CMake split complete. Name the platforms explicitly: macOS and Linux
locally, plus the **Windows** CI lane (the V8 track covers Windows, and CMake
refactors are exactly the kind of change that breaks MSVC quietly), plus the
WASIX toolchain (`EDGE_NAPI_PROVIDER=imports`).

### Phase 4: Focused Source Organization

- Move registry files into `src/binding_registry/`.
- Move binding implementation ownership into `src/bindings/` only where it
  removes a trampoline or central dispatch dependency.
- Keep large binding implementations split when merging would hurt reviewability.
- Keep source moves separate from semantic edits where possible.
- Update includes mechanically.
- Add a build-level guarantee that bindings do not link back to the loader.

Gate:

```sh
make test-quickjs-only TEST_JOBS=4
make test-only TEST_JOBS=4
```

### Phase 5: Documentation and Enforcement

- Finish `test_6_binding_registry.cc` against the final registry shape.
- Add a JS smoke test that asks for every registry name and checks the expected
  non-undefined exports or documented skip behavior.
- Update architecture docs with:
  - target graph;
  - new directory map;
  - "How to add a binding".
- Add or update a cheap source guard for provider-boundary violations under
  shared `src/`.

The desired developer workflow after this plan:

```text
Add one binding file.
Add one EDGE_BINDING(...) manifest line.
Add one source entry in src/bindings/CMakeLists.txt.
Run the registry test and runtime smoke.
```

## Acceptance Criteria

- `internalBinding(name)` resolves through exactly one binding manifest.
- `dispatch.cc`, `ResolveCallbacks`, duplicate loader dispatch, and
  module-loader-owned binding caches are gone.
- The runtime binding name surface matches the Phase 0 fixture on V8 and
  QuickJS.
- `edge_binding_registry` builds and tests without depending on `edge_loader`.
- Root CMake delegates options, provider, vendored dependency, OpenSSL, ICU,
  builtin catalog, and source-list ownership to helper modules or subsystem
  CMake files.
- Adding a binding no longer requires edits in multiple dispatch tables and the
  monolithic root `CMakeLists.txt`.
- Required V8, QuickJS, and WASIX gates are green or explicitly documented with
  known unrelated failures.

## Risks and Mitigations

| Risk | Mitigation |
| --- | --- |
| Cache lifetime changes during env teardown | Unit-test `FinalizeEnv`, run ASAN where available, and exercise worker/messaging tests. |
| Hidden binding initialization order dependencies | Wire the registry behind the existing `internalBinding()` call first, then migrate bindings in small slices. |
| WASIX breakage from source-list or provider CMake movement | Gate WASIX-impacting phases with `quickjs-wasm/build.sh` and Wasmer smoke checks. |
| Loader-specific bindings still need loader services | Add a narrow loader API only for real loader services; do not let the registry depend on the loader. |
| Large source moves create review churn | Do registry and dispatch cleanup before source moves; keep moves mechanical and separate where possible. |
| Alias/shared bindings regress | Represent aliases explicitly and cover them in registry tests. |
| The two legacy dispatch tables expose different name sets, so a manifest built from one drops names from the other | Phase 0 statically diffs both tables; every native-only name gets an explicit manifest-or-private-helper decision before Phase 2. |

## WASIX Verification

For changes under `src/`, provider CMake, generated builtin rules, or binding
source ownership, run the WASIX build before marking the phase complete:

```sh
cd quickjs-wasm && ./build.sh
wasmer package build --check .
wasmer run . -- --version
wasmer run . -- -e "console.log('hello from quickjs')"
```

Use the `wasmer run --net` HTTP smoke when networking, stream, HTTP, TLS, or
listener startup code moves.

## Follow-Ups

These are useful but not part of the essential plan:

- full `src/cli`, `src/runtime`, and `src/loader` source-tree migration;
- deeper `edge_module_loader.cc` split by loader responsibility;
- explicit builtin module manifest/shim pipeline;
- runtime config API extraction from CLI setup;
- repo-wide formatting cleanup.
