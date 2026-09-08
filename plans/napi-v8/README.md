# N-API V8 Refactor Notes

| | | Remarks |
| --- | --- | --- |
| **Status** | 🟢 | Implemented and verified. |
| **Last updated** | 2026-09-07 | Includes the custom V8 11.9.9 build integration. |

## Goal

The `napi/v8` backend should be a thin V8-backed N-API implementation. It should
follow Node's V8 implementation shape from `node/src/js_native_api_v8.cc` where
possible, and borrow the cleanup and diagnostics discipline that already works
well in `napi/quickjs/src`.

The important design rule is:

- `napi_value` is a current-scope V8 local handle.
- `napi_ref` is persistent storage backed by V8 globals.
- Do not make public `napi_value` persistent to work around scope issues.

## What Changed

### Direct `napi_value`

`napi/v8/src/internal/napi_v8_env.h` now uses the Node-style direct local-handle
conversion:

```cpp
inline napi_value JsValueFromV8LocalValue(v8::Local<v8::Value> local) {
  return reinterpret_cast<napi_value>(*local);
}
```

`napi_v8_wrap_value(...)` records diagnostic lifetime data and then returns the
direct local-handle cast. `napi_v8_unwrap_value(...)` reconstructs the
`v8::Local<v8::Value>` with the same memcpy pattern Node uses.

### Real Handle Scopes

N-API handle scopes now hold real V8 scope objects:

- `napi_handle_scope_wrapper__`
- `napi_escapable_handle_scope_wrapper__`

They live under `napi/v8/src/internal/` in files without trailing double
underscores:

- `napi_handle_scope_wrapper.h`
- `napi_handle_scope_wrapper.cc`
- `napi_escapable_handle_scope_wrapper.h`
- `napi_escapable_handle_scope_wrapper.cc`

`napi_escape_handle(...)` uses the V8 escapable scope and records the escaped
value in the parent N-API scope for diagnostics.

### Node-Shaped References And Finalizers

The old mutable `napi_ref__` path was split into Node-like internal classes:

- `napi_ref__`
- `napi_ref_with_data__`
- `napi_ref_with_finalizer__`
- `napi_ref_tracker__`

These live in:

- `napi_ref.h` / `napi_ref.cc`
- `napi_ref_with_data.h` / `napi_ref_with_data.cc`
- `napi_ref_with_finalizer.h` / `napi_ref_with_finalizer.cc`
- `napi_ref_tracker.h` / `napi_ref_tracker.cc`

`napi_create_external(...)`, `napi_wrap(...)`, and `napi_add_finalizer(...)`
now create the appropriate reference type instead of constructing a raw
`napi_ref__` and mutating public fields.

Finalizer references are linked into `env->finalizing_reflist`, and weak V8
callbacks enqueue finalizers through the env before draining them from a
microtask. The helper callback lives in:

- `napi_util.h`
- `napi_util.cc`

### Externals And Type Tags

The external wrapper was moved out of `js_native_api_v8.cc` and renamed to:

- `napi_external_wrapper__`

with files:

- `napi_external_wrapper.h`
- `napi_external_wrapper.cc`

This matches Node's model: `v8::External` points at a wrapper that carries native
data and optional type-tag state.

### Callback Info

Callback info wrappers were extracted from `js_native_api_v8.cc` into internal
classes:

- `napi_function_callback_info__`
- `napi_getter_callback_info__`
- `napi_setter_callback_info__`
- `napi_callback_payload__`
- `napi_accessor_payload__`

Files:

- `napi_function_callback_info.h` / `napi_function_callback_info.cc`
- `napi_getter_callback_info.h` / `napi_getter_callback_info.cc`
- `napi_setter_callback_info.h` / `napi_setter_callback_info.cc`
- `napi_callback_payload.h`

These wrappers expose V8 callback state to N-API without heap-owning
`napi_value` objects.

### Lifetime Tracker

V8 now has QuickJS-style lifetime diagnostics:

- `napi_lifetime_tracker.h`
- `napi_lifetime_tracker.cc`
- `napi_lifetime_macros.h`

CMake/env options:

```sh
NAPI_ENABLE_LIFETIME_TRACKER=ON
NAPI_ENABLE_LIFETIME_PERIODIC_STATS=ON
NAPI_ENABLE_LIFETIME_TAG_STATS=ON
NAPI_ENABLE_LIFETIME_STRING_SYMBOL_DUMP=ON
```

Runtime dump trigger:

```sh
EDGE_TRACE_NAPI_LIFETIME=1
```

The output uses the same section names as QuickJS, including:

- `[napi-lifetime-slots]`
- `[napi-lifetime-scopes]`
- `[napi-lifetime-types]`
- `[napi-lifetime-tags]`
- `[napi-lifetime-scope-values]`
- `[napi-lifetime-strings]`
- `[napi-lifetime-objects]`

The tracker records `napi_value` creation/release by current V8 scope for
diagnostics. It does not persist public `napi_value` handles.

## Build Integration

### Current custom V8 build

EdgeJS uses [custom V8 build 11.9.9](https://github.com/wasmerio/v8-custom-builds/releases/tag/11.9.9)
for the Python fixes. This custom-build release embeds upstream V8
`13.6.233.17`; the release tag and engine version are distinct. The root
`Makefile` prebuilt default and the Linux amd64
Nix development shell must match the pinned N-API backend's version. The Nix
archive has SHA-256
`32d15e5efd1dd19afd48174b1908f7d8813c8433fbfbfe616c06f380319a716d`.

The historical 11.9.2 commands below document the original refactor baseline;
they are not the current build pins. Integration verification is tracked in
[the V8 11.9.9 task note](../quickjs-wasm/development/dev_010_v8_1199_integration/001_edgejs_build_pins.md).

### Original refactor integration

`napi/v8/CMakeLists.txt` now builds the internal source files explicitly and
uses the same generic lifetime tracker options as QuickJS, exposed through the
same `xoption(...)` style.

The root `Makefile` also gained convenience targets:

```sh
make test-native-v8
make test-native-quickjs
make build-edge
```

`napi/Makefile` auto-detects an existing Cargo-produced V8 prebuilt under
`napi/target/debug/build/wasmer-napi-*/out/v8-prebuilt/<version>/<platform>`
before falling back to the CMake build cache. This keeps plain
`make -C napi test-native-v8` usable after the Rust-side V8 prebuilt has already
been materialized. The CMake V8 resolver also discards 0-byte prebuilt archive
cache files before attempting a download, so an interrupted or empty archive is
not treated as a valid extraction source.

## Verification

The following passed after the refactor:

```sh
make -C napi test-native-v8
```

Result: 48/48 V8 N-API tests passed.

```sh
NAPI_V8_DIST_ROOT=/Users/sadhbh/src/dev/edgejs/napi/target/debug/build/wasmer-napi-dc5f75328e8ed969/out/v8-prebuilt/11.9.2/darwin-arm64 \
make -C napi test-native-v8 EXTRA_CMAKE_ARGS='-DNAPI_ENABLE_LIFETIME_TRACKER=ON -DNAPI_ENABLE_LIFETIME_PERIODIC_STATS=ON -DNAPI_ENABLE_LIFETIME_TAG_STATS=ON -DNAPI_ENABLE_LIFETIME_STRING_SYMBOL_DUMP=ON'
```

Result: 48/48 V8 N-API tests passed.

```sh
make -C napi test-native-quickjs
```

Result: 48/48 QuickJS N-API tests passed.

```sh
NAPI_V8_DIST_ROOT=/Users/sadhbh/src/dev/edgejs/napi/target/debug/build/wasmer-napi-dc5f75328e8ed969/out/v8-prebuilt/11.9.2/darwin-arm64 \
NAPI_ENABLE_LIFETIME_TRACKER=ON \
make build-edge EXTRA_CMAKE_ARGS='-DNAPI_ENABLE_LIFETIME_TRACKER=ON -DNAPI_ENABLE_LIFETIME_PERIODIC_STATS=ON -DNAPI_ENABLE_LIFETIME_TAG_STATS=ON -DNAPI_ENABLE_LIFETIME_STRING_SYMBOL_DUMP=ON'
```

Result: `edge` built successfully.

```sh
./build-edge/edge -e "console.log('edge v8 napi ok')"
```

Result:

```text
edge v8 napi ok
```

Whitespace checks also passed:

```sh
git diff --check
git -C /Users/sadhbh/src/dev/edgejs/napi diff --check
```

### CI Build Fix

The GitHub `v8-native-linux` and `v8-native-macos` jobs compile with lifetime
tracking disabled by default. `napi_ref.cc` still called the generic lifetime
template, but only included `napi_lifetime_macros.h`; with the feature disabled,
that header did not declare `v8impl::detail::napi_lifetime__`.

The fix was to include `napi_lifetime_tracker.h` directly in `napi_ref.cc`.
That header provides the real tracker API when enabled and no-op template
methods when disabled.

Verified with:

```sh
make build-napi CMAKE_BUILD_TYPE=Release JOBS=4 \
  EXTRA_CMAKE_ARGS='-DNAPI_ENABLE_LIFETIME_TRACKER=OFF -DNAPI_ENABLE_LIFETIME_PERIODIC_STATS=OFF -DNAPI_ENABLE_LIFETIME_TAG_STATS=OFF -DNAPI_ENABLE_LIFETIME_STRING_SYMBOL_DUMP=OFF'
ctest --test-dir /Users/sadhbh/src/dev/edgejs/build-napi-v8 --output-on-failure -R '^napi_v8\.'
```

Result: default/no-tracker V8 build completed, and 48/48 V8 N-API tests passed.

## Continuing Rule

For future V8 N-API work:

- Use `napi/quickjs/src` as the Edge behavior and diagnostics reference.
- Use `node/src/js_native_api_v8.cc` as the V8 ownership/reference model.
- Keep V8 implementation thin: use V8's local handles, handle scopes, globals,
  weak callbacks, and finalizer behavior directly.
- Put new embedding helper classes under `napi/v8/src/internal/`.
- Name classes with the internal `napi_***__` convention, but keep filenames
  clean, for example `napi_ref.h`, not `napi_ref__.h`.

## Shared Allocator And Lifetime Plumbing

The QuickJS fixed-block allocator is now shared at:

```text
napi/lib/src/napi_allocator.h
```

Both QuickJS and V8 include `napi/lib/src` from CMake. The allocator exposes
`napi_allocator_lifetime__<T, Owner>` hooks so engine-specific lifetime trackers
can observe allocation and release without making the allocator depend on either
backend.

Common lifetime utility code that was repeated between the engines is now under:

```text
napi/lib/src/napi_lifetime_tracker.h
napi/lib/src/napi_lifetime_tracker.cc
```

That shared layer owns the generic counters, env-flag parsing, monotonic clock
helper, output table formatting, string/object aggregation, tag table rendering,
slot/type summary rows, and the common lifetime dump macro. The backend tracker
files are now engine adapters: QuickJS supplies JSValue tag/string/object
inspection and scope scans; V8 supplies `v8::Local` classification, ref
snapshotting, and V8 scope bookkeeping. The printed output stays intentionally
identical between engines.

The same shared-source directory now also contains small engine-neutral
mechanics:

- `napi_error_state.{h,cc}` centralizes `napi_extended_error_info` storage and
  message lifetime.
- `napi_periodic_gate.{h,cc}` centralizes periodic diagnostic gating.
- `napi_text.{h,cc}` centralizes pure decimal BigInt word conversion and UTF-8
  / Latin-1 helper logic used by QuickJS.
- `napi_typedarray_metadata.{h,cc}` centralizes N-API typed-array constructor
  names.

The repeated lifetime CMake option ladder moved to
`napi/lib/cmake/NapiLifetimeOptions.cmake`; backend CMake files keep only their
engine build-test option plus `napi_define_lifetime_options(...)`.

For V8, `napi_env__` now owns typed allocator pools. N-API helper objects that
belong to an env are allocated through `env->allocate<T>(...)` and released via
`env->release(ptr)`, including refs, finalizer refs, cleanup hooks, deferred
promises, callback payloads, external wrappers, buffer records, module-wrap
records, serializer/deserializer contexts, and handle scopes. The lifetime
tracker reports those allocator-backed types in `[napi-lifetime-types]`;
aggregate `napi_ref.*` counters include
`napi_ref__`, `napi_ref_with_data__`, and `napi_ref_with_finalizer__`.

External backing-store hints are intentionally not env-allocator owned. Their
parent lifetime is the V8 backing store, which can outlive `napi_env__` during
isolate teardown. V8 therefore owns deletion through the backing-store deleter,
while `napi_env__` keeps a registry so env teardown can run pending finalizers,
mark outstanding hints detached from the env, and record matching lifetime
release events.

Remaining raw allocations in the V8 backend are deliberately outside this
env-owned model: `napi_env__` itself, V8-owned backing-store hint shells,
transient isolate scope/bootstrap objects, V8 platform task records, V8-owned
cached data, interrupt request payloads, and serialized clone payloads returned
as opaque API data.

## Related Notes

Per-class documentation lives under:

```text
plans/napi-v8/classes/
```

A mechanical comparison with Node's V8 N-API implementation lives at:

```text
plans/napi-v8/node-vs-napi.md
```

V8-backed EdgeJS node-test investigations live at:

```text
plans/napi-v8/development/node-tests.md
```

The original implementation notes are still under:

```text
plans/quickjs-wasm/development/dev_004_v8_napi_lifetime_refactor/
```
