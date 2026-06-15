# Bytecode Precompilation (Sidecar Code Caches)

## Goal

Cut process startup time by skipping JS parse/compile for both CommonJS and
ES modules. Both engines serialize compiled code — V8 via `ScriptCompiler`
code caches, QuickJS (quickjs-ng) via `JS_WriteObject`/`JS_ReadObject`
bytecode — and the serialized form is stored in a **per-directory
`__edgecache__/` subdirectory** next to the source, PEP 3147 (`__pycache__`)
style:

```
src/app.js                                  the source (unchanged, authoritative)
src/__edgecache__/app.js.v8-13-6.jsc        the cache (full filename + short engine tag)
```

A short engine tag — `v8-<major>-<minor>` / `qjs-<major>-<minor>` (e.g.
`v8-13-6`, `qjs-0-14`) — is baked into the cache filename, so different engines
and major.minor versions coexist in the same `__edgecache__/`. The header still
carries the *full* engine version for the staleness check, so two patch builds
sharing a major.minor reuse one filename and the header rejects + rewrites the
stale one (correct, just a recompile — not wrong bytecode). The full
source filename (extension kept) keys the entry (`app.js` → `app.js.<tag>.jsc`),
so it is unique within the directory and `.js`/`.mjs`/`.cjs` siblings never
collide. The cache lives in the
tree (so it travels with the bundle and works read-only in prod when
precompiled); a read-only source tree silently skips the write, like PEP 3147.

The consolidated builtins cache is separate: one file next to the binary
(`<exec-path>.builtins.v8b` / `.qjsb`), not a per-directory `__edgecache__`.

## Behavior

Two independently-gated caches:

- **Builtins (lib/) cache — ON by default.** Our own bytecode (a fixed set,
  validated by engine tag + per-builtin source hash, read-only-safe) is the
  biggest startup win at minimal risk, so it is consumed and written-on-first-
  run for everyone, into one file next to the binary. Ship the prebuilt file
  (`make precompile-builtins`) so the first run / read-only installs benefit
  too.
- **Per-file user sidecars — OFF by default (opt-in).** Arbitrary user code is
  less battle-tested, so the per-file `__edgecache__/` sidecars are only read
  or written when explicitly enabled. Opt in with `--bytecode-cache`,
  `EDGE_BYTECODE_CACHE=1` (any truthy value), or `--precompile`.

- **Consume:** when caching is active and a valid sidecar/builtin entry exists,
  the engine consumes it instead of parsing the source. Invalid, stale, or
  corrupted entries are silently ignored (normal compile).
- **Write-on-first-run:** when caching is active and no valid entry exists, the
  runtime compiles eagerly to a bytecode handle, persists it atomically (temp
  file + rename), and executes from the same handle — the source is parsed
  exactly once. Failures (read-only filesystem, permissions) are silent.
- **Explicit precompile:** `edge --precompile <file|dir>...` (implies user
  sidecars on) walks the paths and compiles every eligible file **without
  executing any module body**: `.cjs` always (CJS shape), `.mjs` always
  (module shape), `.js` per its nearest `package.json` `type` (an ESM-syntax
  `.js` in a commonjs scope is retried with the module shape — mirroring
  runtime detect-module). `node_modules` is included.
- **Kill switch:** `--no-bytecode-cache` (CLI) or `EDGE_BYTECODE_CACHE=0`
  (env) disable **everything** including the builtins cache. `--check`
  disables implicitly (a syntax check must not write to the tree). CLI flags
  win over the env var. `--precompile` conflicts with `--check/--eval/--test/
  --watch/--interactive/--run/--no-bytecode-cache` (exit 9).
- **Tracing:** `EDGE_BYTECODE_CACHE_TRACE=1` prints `hit/miss/write/remove`
  and `builtin-*`/`builtins-*` lines to stderr (used by tests).

## Architecture: JSSource + bytecode handles

The unofficial-NAPI layer models sources as a first-class type instead of
threading cache parameters through each function:

```c
typedef struct unofficial_napi_js_source {  // exactly one of:
  napi_value text;   //   JS string source
  void* bytecode;    //   opaque bytecode handle
} unofficial_napi_js_source;
```

Handles are created by two verbs and consumed by every script-accepting API
(`contextify_run_script`, `contextify_compile_function`,
`contextify_compile_function_for_cjs_loader`,
`module_wrap_create_source_text`):

- `unofficial_napi_bytecode_compile(text, filename, shape, params, ...)` —
  **eager** compile (V8 `kEagerCompile`, so inner functions are serialized
  too) producing a handle that holds the live compiled artifact plus the
  serializable bytes. On compile failure, `can_parse_as_module_out` reports
  the detect-module signal without a second parse.
- `unofficial_napi_bytecode_deserialize(bytes, text, filename, shape, ...)` —
  restores a live artifact from persisted bytes (V8: `kConsumeCodeCache`
  compile; QuickJS: `JS_ReadObject`). Rejection is synchronous:
  `*rejected_out = true` and no handle — the caller falls back to text and
  drops the sidecar.
- `unofficial_napi_bytecode_serialize(handle)` — bytes for persistence.
- `unofficial_napi_bytecode_release(handle)`.

Shapes: `script` (vm.Script), `cjs_function` (function compiled with params —
the CJS wrapper), `module` (ES module). A handle is only usable by APIs
compiling the same shape; the sidecar header records it (flags bit 0 = CJS,
bit 1 = ESM) and cross-shape consumption is rejected at validation.

Engine notes:
- V8 code caches are consumed **against the original source text**, so handles
  retain the text — giving every consumer automatic fallback.
- QuickJS payloads are **fully self-validating**, mirroring everything V8's
  CachedData checks natively: the provider writes a 40-byte `QJSB` header
  `[magic, version, shape u8, reserved, source XXH3, params XXH3, filename
  XXH3, payload XXH3]` at serialize time and rejects any mismatch at
  deserialize. shape/source/params reject bytes compiled from a different
  compile shape, a different source, or a different CJS parameter list (a
  whole-script cache fed to `compileFunction`, or `params:['a']` consumed as
  `['x']` — all of which V8 rejects too). The payload hash guards corruption
  (`JS_ReadObject` is not hardened). The filename hash guards relocation
  (QuickJS bytecode embeds the compile-time path/URL — `import.meta.url` would
  silently go stale); a stored hash of 0 is unenforced, written by
  `vm.SourceTextModule#createCachedData` because Node numbers vm module
  identifiers (`vm:module(N)`).
- Because the QJSB header covers source identity, **the NAPI providers
  self-validate untrusted vm cachedData directly** — Edge hands user buffers
  straight to `bytecode_deserialize` and trusts `rejected_out`. There is no
  Edge-side cachedData wrapper on either engine (V8 CachedData validates
  natively and stays Node-parity raw; QuickJS validates via QJSB).
  Sidecar/builtins containers also record their own source hash, so their
  payloads are double-checked but never wrapped.
- QuickJS `JS_ReadObject` of module bytecode resolves import requests eagerly
  at read time; since this embedding links modules explicitly afterwards
  (`JS_SetModuleRequestModule`), `deserialize` installs a **stub module
  loader** (RAII guard returning no-op `JS_NewCModule` entries) around the
  read. Stubs are keyed by specifier strings, real modules by full `file://`
  URLs — no collision; explicit linking overrides resolution.
- QuickJS `import.meta`/dynamic-import hooks are keyed by `JSModuleDef*` →
  record, so bytecode-restored modules behave identically once registered.
  Caveat discovered on the real-app validation: quickjs's `js_import_meta`
  resolves the executing module **by name** through `ctx->loaded_modules`, so
  stub modules must NOT be registered under real URLs — they get unique
  `edge-bytecode-stub:N` names (regression-tested: import.meta inside an
  imported cached module).
- `module_wrap_create_source_text`'s 8th param is a host-defined-option
  **Symbol only** again (load-bearing for dynamic `import()` dispatch); cached
  data travels inside the JSSource.

## Sidecar format

Fixed 48-byte little-endian header + engine tag + opaque self-validating
engine payload (`src/edge_bytecode_cache.h`). The container is fully
engine-agnostic — anything engine-specific (QuickJS filename + corruption
guards) lives inside the payload, written/parsed by the provider:

| field | notes |
|---|---|
| magic `EDGEJSBC` | |
| format_version u32 | bump when a compile shape or the encoding changes (wrapper text, params, shebang handling) |
| flags u32 | bit0 = CJS function shape, bit1 = ESM module shape (one-hot; the script shape never sidecars) |
| source_len u64 + source_hash u64 | XXH3-64 over the exact source string handed to the compiler |
| payload_len u64 | pins the file structure — truncated/padded files reject |
| tag_len u32 + engine tag | `v8-<EDGE_EMBEDDED_V8_VERSION>` / `qjs-ng-<EDGE_EMBEDDED_QUICKJS_VERSION>` |
| payload | rest of file: V8 raw CachedData / QuickJS `QJSB`-headered bytecode |

Validation order: magic → version → flags → structure (payload_len) → tag →
source hash; payload integrity/identity then validates inside the engine
provider at deserialize. Any miss = "no cache". An engine-rejected-but-
header-valid sidecar is deleted so the next run rewrites it.

## Implementation map

No JS under `lib/` was touched. All logic is C++:

- `src/edge_bytecode_cache.{h,cc}` — container format, validation, hashing,
  atomic writes, enable state.
- `src/edge_module_loader.cc` — CJS hook in
  `ContextifyCompileFunctionForCJSLoaderCallback` (deserialize sidecar →
  JSSource, or compile→serialize→write, then execute the same handle); vm
  bindings (`vm.Script` ctor/`createCachedData`, `vm.compileFunction`) build
  JSSource handles and set `cachedData`/`cachedDataProduced`/
  `cachedDataRejected` themselves.
- `src/internal_binding/binding_module_wrap.cc` — ESM hook in `ModuleWrapCtor`
  (gated to default-loader, `file://`-backed source-text modules; vm modules
  with user cachedData take a deserialize path that fixed the previously
  silent no-op of `vm.SourceTextModule({cachedData})` on V8).
- `src/edge_url.{h,cc}` — `edge_url::PathToFileURLString()` (byte-identical to
  the JS loader's `pathToFileURL`; QuickJS bakes the URL into module
  bytecode).
- `src/edge_precompile.{h,cc}` — uniform driver: `bytecode_compile` →
  `bytecode_serialize` → `WriteSidecar` per shape.
- Providers: `napi/v8/src/unofficial_napi_contextify.cc` (BytecodeRecord +
  shape-dispatched eager compile/consume; artifact reuse for the CJS loader
  fast path), `napi/quickjs/src/internal/napi_contextify.cc` +
  `napi_module_wrap.cc` (record in `napi_bytecode.h`; stub-loader guard;
  `create_cached_data` now real — backs `vm.SourceTextModule#createCachedData`).
- Wasm bridge: shims updated + 4 new `snapi_bridge_unofficial_bytecode_*`
  entries with a dedicated handle table (`napi/src/napi_bridge_init.cc`,
  `snapi.rs`, `guest/napi.rs`).

## Tests

- `tests/runners/test_7_bytecode_cache_phase05.cc` (gtest, V8 lane): header
  round-trip, every corruption mode, shape cross-rejection, spawned-binary
  e2e. XXH3 known vectors; the container-level payload-corruption assertion
  is engine-conditional (V8 writes no payload hash).
- `tests/runners/test_8_builtin_bytecode.cc` (gtest): builtins container
  round-trip, bad magic/version/tag, truncation, trailing garbage,
  corrupt-entry drop semantics, cache-file path shape.
- `scripts/test-bytecode-cache.sh` (both engines): `make test-bytecode-cache`
  / `make test-bytecode-cache-quickjs` — 28 scenarios covering CJS + ESM
  write/consume, import chains, dynamic import, `import.meta`, TLA,
  corruption/staleness, opt-outs, read-only trees, `type: module` scoping,
  detect-module, `--check` cleanliness, `vm.Script`/`vm.compileFunction`/
  `vm.SourceTextModule` cachedData round-trips, and the builtins cache
  (write/hit, corrupt-rewrite, opt-outs, read-only bin dir, output parity,
  worker smoke — the last V8-only while QuickJS worker_threads hangs).

## Benchmark

`benchmarks/bench-bytecode-cache.sh` (hyperfine; `EDGE_BIN` selects engine)
now runs **CJS and ESM lanes** over deterministic ~3.4 MB twin apps generated
by `benchmarks/generate-bytecode-workload.mjs` (`gen/` CJS, `gen-esm/` ESM).
Results land in `benchmarks/results/bytecode-cache-<engine>-<lane>.*`.

Measured 2026-06-11 after phase 3 (Apple Silicon, low-noise runs; the warm
column includes the builtins cache, the cold column runs
`--no-bytecode-cache`):

| engine / lane | cold | warm | speedup |
|---|---|---|---|
| V8 / CJS | 194.0 ms | 105.0 ms | 1.85× |
| V8 / ESM | 183.9 ms | 128.7 ms | 1.43× |
| V8 / empty startup | 69.1 ms | 33.8 ms | 2.04× |
| QuickJS / CJS | 178.2 ms | 88.4 ms | 2.02× |
| QuickJS / ESM | 202.8 ms | 118.6 ms | 1.71× |
| QuickJS / empty startup | 81.4 ms | 35.7 ms | 2.28× |

Real-app validation (Astro server, 7.3 MB ESM + React CJS, 167 sidecars,
time-to-first-HTTP-response, 8 runs each): QuickJS 525.1 → 402.6 ms
(**−122 ms, 23%**); V8 204.4 → 195.3 ms (**−9 ms** — flipped from the
pre-phase-3 net-zero once sidecar reads got cheap and builtins stopped
compiling). Per-stage: QuickJS to-listen 193 → 143 ms, first-request
332 → 260 ms.

## Phase 3: fast sidecar reads + consolidated builtins cache

Shipped 2026-06-11. Two independent wins:

**A. Sidecar read path (container format v2).**
- Container hashing switched FNV-1a → **XXH3-64** (vendored official xxhash
  v0.8.2 single-header at `deps/xxhash/` and
  `napi/quickjs/src/internal/xxhash.h`; zstd's bundled xxhash hard-defines
  `XXH_NO_XXH3`, hence the copies). Ownership split: Edge owns the
  engine-agnostic container (source identity + structure); the QuickJS
  provider owns its payload's full self-validation (`QJSB` header: shape,
  source, params, filename, and corruption hashes), mirroring what V8's
  CachedData does internally — so there is no Edge-side cachedData wrapper on
  either engine. `kFormatVersion` is 4 — older sidecars recompile once and
  rewrite.
- The payload hash is engine-conditional like the filename hash: V8 writes 0
  and skips the check (CachedData self-validates; `rejected` already falls
  back), QuickJS keeps it (its reader is not hardened).
- `ReadSidecar` does one sized `fread` into `SidecarPayload` and validates in
  place; `DecodeSidecar` returns payload offset/length instead of copying.
  Measured on the Astro app (167 hits): read+hash dropped **~20 → 5.6 ms** on
  V8 (3.8 MB of payloads) and **~40 → 3.7 ms** on QuickJS (9.6 MB).

**B. Builtins consolidated bytecode cache (`src/edge_builtin_bytecode.{h,cc}`).**
- One file next to the binary: `<exec-path>.builtins.v8b` / `.qjsb` (magic
  `EDGEJSBB`; entries `[kind, id_len, payload_len, source_xxh3, payload_xxh3,
  id, payload]`). Kinds 0–5 pin the compile shape: four cjs-function
  parameter shapes (per-context / bootstrap-realm / bootstrap-main / lazy
  builtin) plus the two `EvaluateJsModule` wrapper-script shapes; the source
  hash covers the exact string handed to the compiler, so wrapper or
  parameter drift self-invalidates.
- All three builtin compile paths consult it (`BuiltinsCompileFunctionCallback`,
  `ExecuteBuiltinFromNative`, and the builtin branch of `EvaluateJsModule`,
  the latter via `unofficial_napi_contextify_run_script` with a null sandbox);
  any bytecode failure falls back to the original text path and its exact
  error shaping. ~96 entries (≈1.5 MB V8 / ≈3.6 MB QuickJS) by an empty boot.
- The store is process-global (mutex; workers record into the same map);
  `FlushIfDirty()` runs once at `RunWithFreshEnv` teardown with an atomic
  tmp+rename. Read-only install dirs are a silent no-op. Same opt-outs as
  sidecars (`--no-bytecode-cache`, `EDGE_BYTECODE_CACHE=0`, `--check`).
- V8's generic `unofficial_napi_contextify_compile_function` gained the
  artifact fast path the cjs_loader variant already had (guarded on context,
  params, offsets, filename, and host-defined-option symbol incl. the
  relaxed CJS default), so builtin hits consume the code cache once, not
  twice — empty-startup warm went 47 → 34 ms with it.
- Trace lines (`EDGE_BYTECODE_CACHE_TRACE=1`): `builtin-hit/builtin-miss/`
  `builtins-load/builtins-write/builtins-write-failed/builtins-load-failed`.

## Known limitations / follow-ups

- **V8 flag drift thrashes the builtins file**: V8's flags hash is part of
  CachedData validation, so alternating flag sets (e.g. `--prof` on/off)
  reject + rewrite all builtin entries each run. Folding the V8 flags hash
  into the engine tag is a follow-up.
- Multi-process first-boot races on the builtins file are last-writer-wins
  (both writers produce valid files; losers recompile next run).
- `worker_threads` hangs on QuickJS (parent never receives messages) —
  pre-existing engine issue independent of the cache (reproduces on a clean
  tree with `--no-bytecode-cache`); the worker e2e scenario is V8-only until
  that is fixed.
- QuickJS payloads are keyed to the absolute path/URL (stack-trace and
  `import.meta.url` fidelity) via the provider's `QJSB` filename hash;
  relocated trees recompile once and rewrite (e2e-covered). V8 sidecars are
  relocatable.
- On V8, the ESM consume path deserializes the cache twice per module (once at
  `deserialize` for synchronous rejection, once inside `create_source_text`
  with the loader's host-defined-options) — still far cheaper than a parse;
  optimization candidate.
- vm semantics now match Node where they previously didn't:
  `vm.Script({cachedData})` validates and reports `cachedDataRejected`
  honestly (any ArrayBufferView flavor accepted, incl. Float16Array/DataView);
  `vm.SourceTextModule({cachedData})` consumes for real and throws
  `ERR_VM_MODULE_CACHED_DATA_REJECTED` on mismatch (the constructor throw
  lives in the C++ binding since the ported lib omits the JS-side check).
  `test-vm-cached-data.js`, `test-vm-createcacheddata.js` and
  `test-vm-module-cached-data.js` pass on both engines (not CI-gated).
  `script.run*` still compiles from text (as before); QuickJS cannot
  engine-validate `vm.compileFunction` cachedData against mismatched params
  (bytecode encodes them) — sidecar paths always use fixed params.
- The Node test harness lanes run with `EDGE_BYTECODE_CACHE=0` so
  write-on-first-run does not spray sidecars into `test/fixtures/`. Both lanes
  were verified green with the cache enabled too.
- Node's `module.enableCompileCache()` stubs remain disabled; sidecars
  supersede them.
