# N-API integration

Status: local implementation and validation complete; PR CI pending.
Owner: N-API worker. Depends on coordinator engine merge.
Write ownership: N-API source/tests/build files except quickjs/deps/quickjs and
this note. Audit upstream API changes and remove obsolete adaptations only when
behavioral equivalence is demonstrated. Compare V8 intent before implementation.
Verify native Release/Debug N-API suites; preserve existing test exclusions and
report unrelated failures. Coordinator owns commits/gitlinks/pushes/PRs.

## API audit and action plan

Compared baseline QuickJS `7b8ab77` with upstream `6d46d07` and the V8
allocation/finalizer implementation before editing.

- Upstream `6a2c5f3` replaces external ArrayBuffer free callbacks with realloc
  callbacks and adds `max_len` to `JS_NewArrayBuffer`. Pass zero for N-API's
  fixed-length buffers. Runtime-owned buffers can use `js_realloc_rt`;
  embedder-owned memory must reject nonzero reallocation because its allocator
  is unknown. Same-length transfer remains supported.
- Upstream `4369dd6` clears the backing callback and opaque pointer after
  detach, preventing a second callback at collection. Delete N-API's old
  `begin_detach`/`end_detach` coordination and `JS_GetArrayBufferFreeInfo`
  dependency; finalize and release hint metadata during the sole callback.
  Coordinator will remove the now-unused fork engine API.
- SharedArrayBuffer allocation hooks remain supported; only the new zero
  `max_len` argument is needed.
- `JS_GetClassName` still returns a duplicated atom; existing release is correct.
  N-API does not use removed upstream `JSClass` or `js_realloc2` declarations.
  Existing private-symbol caller can use the upstream implementation unchanged.
- Upstream `d5a59c3` fixes async iterator close ordering; remove the stale
  QuickJS-specific `[false,true]` test expectation and use `[true,true]` on both
  engines.
- Upstream `1a5ee30` adds `JS_PromiseMarkAsHandled`. Replace the existing
  unofficial N-API no-op with this API after validating the argument is a
  promise, and verify the rejection callback receives the handled event.

Add focused ownership regressions for runtime-owned transfer resizing and
external buffer transfer/detach/finalization. After the engine is ready, build
and run the full Release and Debug N-API suites with their existing exclusion.

## Implementation and validation

Source adaptations and four focused regressions are implemented. Release and
Debug builds completed after the coordinator confirmed the engine merge compiles.
Logs: `/private/tmp/quickjs-upstream-sync/napi-{release,debug}-build.log`.
External ArrayBuffers retain fixed-length ownership: a requested nonzero resize
fails without freeing or modifying the embedder's storage. Ordinary N-API
ArrayBuffers can transfer to a different size via the runtime allocator.

Commands from the isolated N-API checkout:

```sh
make build-napi-quickjs CMAKE_BUILD_TYPE=Release JOBS=4
make build-napi-quickjs CMAKE_BUILD_TYPE=Debug BUILD_NAPI_QUICKJS_DIR=/private/tmp/quickjs-upstream-sync/edgejs/napi/build-napi-quickjs-debug JOBS=4
ctest --test-dir build-napi-quickjs --output-on-failure -R '^napi_quickjs\.' -E 'SandboxGlobalThisAndMarkerAreNotEnumerableForDeepFreeze' -j4
ctest --test-dir build-napi-quickjs-debug --output-on-failure -R '^napi_quickjs\.' -E 'SandboxGlobalThisAndMarkerAreNotEnumerableForDeepFreeze' -j4
```

Both builds passed; both suites passed **95/95** tests, retaining the existing
Makefile exclusion. Test logs are
`/private/tmp/quickjs-upstream-sync/napi-{release,debug}-test.log`. No runtime
failure required a troubleshooting note. The class documentation for
`napi_external_backing_store_hint__` now reflects the removed detach workaround.
Coordinator owns remaining gitlink changes, commits, PRs, and CI validation.

Because the new ownership/handled-promise tests are shared, also configured the
V8 11.9.9 Release provider and built `napi_v8_test_33_typedarray` and
`napi_v8_test_35_promise`. The focused V8 suite passed **11/11** tests. Logs are
`/private/tmp/quickjs-upstream-sync/napi-v8-{configure,build,test}.log`.

After the coordinator fixed the engine lazy-stack descriptor success check,
rebuilt both QuickJS configurations and reran both suites: **95/95 passed in
Release and 95/95 passed in Debug**. Final logs are
`/private/tmp/quickjs-upstream-sync/napi-{release,debug}-final-test.log` and the
incremental build logs are `napi-{release,debug}-rebuild.log` in that directory.

Cache audit: upstream's bytecode format is now version 28. Existing
`bytecode_deserialize` validates its QJSB envelope, then rejects and clears any
`JS_ReadObject` exception (including engine version mismatch); callers using
`compile_on_reject` recompile from source. Existing contextify tests cover cache
round-trip, mismatched-source rejection, empty-cache fallback, and module
state/hook APIs. The known upstream serialization loss of module import
attributes/phase remains outside this synchronization's wire-format changes.

## Consumed dependency commit retention

A later gitlink audit found that N-API `main` consumes QuickJS commit
`ff1471cf525483ea1e5b8030d5ecf274ed00eb70` from
`codex/error-derived-stack-filter`, which had not reached the fork's `master`.
The coordinator is preserving it in the engine integration: derived Error
constructor filtering, borrowed receiver state, and CallSite `getThis` /
`getTypeName`, with receiver ownership cleared along upstream CallSite cleanup
paths. Both N-API configurations rebuilt successfully and their full suites
passed against this source: **95/95 Release and 95/95 Debug**. No N-API source
adaptation was necessary. Logs:
`/private/tmp/quickjs-upstream-sync/napi-{release,debug}-final-rebuild.log` and
`/private/tmp/quickjs-upstream-sync/napi-{release,debug}-consumed-commit-test.log`.
