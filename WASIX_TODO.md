# WASIX TODO

This file tracks temporary compatibility workarounds used to get `ubi` building for WASIX.
Items here should be replaced with real WASIX implementations, proper feature gates, or upstream fixes.

## ICU

- Replace the temporary `tzname` / `timezone` / `daylight` UTC stub in [ubi/cmake/wasix-libuv-compat.h](/home/theduke/dev/github.com/wasmerio/ubi/ubi/cmake/wasix-libuv-compat.h) with a proper WASIX timezone integration once the runtime/sysroot exposes a supported API.
- Validate that the embedded ICU data path is the right long-term packaging model for WASIX. Alternatives to consider:
  - dedicated `icudata` object generation at build time
  - runtime loading from a colocated data file
  - upstreaming a canonical WASIX ICU packaging flow
- Audit the full vendored ICU source build and trim it to the minimal set of libraries/features actually needed by `ubi` if build time or wasm size becomes a problem.

## libc / sysroot gaps

- Replace the `fork()` stub in [ubi/cmake/wasix-libuv-compat.h](/home/theduke/dev/github.com/wasmerio/ubi/ubi/cmake/wasix-libuv-compat.h) with either:
  - a proper WASIX process model implementation, or
  - an explicit feature disable path in `ubi` where `fork`-style behavior is unsupported.
- Replace the `if_nametoindex()`, `if_indextoname()`, and `getservbyport_r()` stubs in [ubi/cmake/wasix-libuv-compat.h](/home/theduke/dev/github.com/wasmerio/ubi/ubi/cmake/wasix-libuv-compat.h) with real WASIX-compatible implementations or upstream fixes in the sysroot/libc.
- Revisit the scheduler and thread-name shims in [ubi/cmake/wasix-libuv-compat.h](/home/theduke/dev/github.com/wasmerio/ubi/ubi/cmake/wasix-libuv-compat.h) and replace them with real support when available.
- Revisit the `ptsname()` stub in [ubi/cmake/wasix-libuv-compat.h](/home/theduke/dev/github.com/wasmerio/ubi/ubi/cmake/wasix-libuv-compat.h) once PTY support is clearer on WASIX.

## UBI feature stubs

- Revisit the temporary zero-return behavior for process memory APIs in [ubi/src/ubi_process.cc](/home/theduke/dev/github.com/wasmerio/ubi/ubi/src/ubi_process.cc). These currently avoid unresolved imports for `uv_get_available_memory`, `uv_get_constrained_memory`, and `uv_resident_set_memory`, but should either report real values or be explicitly feature-gated.
- Revisit the `getgroups()` guard in [ubi/src/internal_binding/binding_credentials.cc](/home/theduke/dev/github.com/wasmerio/ubi/ubi/src/internal_binding/binding_credentials.cc) once the WASIX libc story for group APIs is clearer.

## Build system

- Stop `ubi/scripts/setup-wasix-deps.sh` from mutating cloned deps during normal builds; switch to pinned revisions or explicit update steps.
- Reduce noisy WASIX compatibility warnings in the build once the functional gaps are resolved.

## N-API harness

- Revisit getter/setter callback dispatch in [napi/wasmer/src/napi_bridge_init.cc](/home/theduke/dev/github.com/wasmerio/ubi/napi/wasmer/src/napi_bridge_init.cc). It currently distinguishes property getters from setters by callback arity when a single N-API property descriptor carries both, which is sufficient for now but should become an explicit callback-kind bridge.
- Replace the temporary no-op `napi_add_env_cleanup_hook()` / `napi_remove_env_cleanup_hook()` bridge in [napi/wasmer/src/lib.rs](/home/theduke/dev/github.com/wasmerio/ubi/napi/wasmer/src/lib.rs) with a real cleanup-hook registry that invokes guest callbacks during env teardown.
