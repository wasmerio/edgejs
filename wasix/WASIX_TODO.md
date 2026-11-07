# WASIX TODO

This file tracks temporary compatibility workarounds used to get `ubi` building for WASIX.
Items here should be replaced with real WASIX implementations, proper feature gates, or upstream fixes.

## ICU

- Replace the temporary `tzname` / `timezone` / `daylight` UTC stub in [wasix/src/wasix_compat.h](/home/theduke/dev/github.com/wasmerio/ubi/wasix/src/wasix_compat.h) with a proper WASIX timezone integration once the runtime/sysroot exposes a supported API.
- WASIX builds no longer embed ICU data (`EDGE_EMBED_ICU_DATA=OFF`). ICU loads individual item files from `/usr/share/edge-icu/icudt78l/`, staged into `<build>/icu/` and mapped by the package manifests. Still open:
  - measure the per-item linear-memory cost of ICU's mmap path under WASIX, and switch to `U_HAVE_MMAP=0` if it is high
  - trim the staged item set if package file count or size becomes a problem
  - upstreaming a canonical WASIX ICU packaging flow
- Audit the full vendored ICU source build and trim it to the minimal set of libraries/features actually needed by `ubi` if build time or wasm size becomes a problem.

## libc / sysroot gaps

- Native asynchronous and synchronous spawn accept `detached: true` as an
  ordinary WASIX spawn. No new session or process group is created, so this
  does not provide Unix group cancellation. Remove this fallback when WASIX
  supports the corresponding spawn attributes and group signals.
- Native `process.kill()` calls `proc_signal` directly on WASIX because the
  released libc `kill()` returns syscall errors without setting `errno`.
  Missing-PID errors also require a host that returns `ESRCH` from `proc_signal`.
  This allows callers to fall back from group signals to an individual PID;
  it does not guarantee termination of all descendants.

- Replace the `fork()` stub in [wasix/src/wasix_compat.h](/home/theduke/dev/github.com/wasmerio/ubi/wasix/src/wasix_compat.h) with either:
  - a proper WASIX process model implementation, or
  - an explicit feature disable path in `ubi` where `fork`-style behavior is unsupported.
- Replace the `if_nametoindex()`, `if_indextoname()`, and `getservbyport_r()` stubs in [wasix/src/wasix_compat.h](/home/theduke/dev/github.com/wasmerio/ubi/wasix/src/wasix_compat.h) with real WASIX-compatible implementations or upstream fixes in the sysroot/libc.
- Revisit the scheduler and thread-name shims in [wasix/src/wasix_compat.h](/home/theduke/dev/github.com/wasmerio/ubi/wasix/src/wasix_compat.h) and replace them with real support when available.
- Revisit the `ptsname()` stub in [wasix/src/wasix_compat.h](/home/theduke/dev/github.com/wasmerio/ubi/wasix/src/wasix_compat.h) once PTY support is clearer on WASIX.

## UBI feature stubs

- Revisit the temporary zero-return behavior for process memory APIs in [wasix/src/wasix_compat.cc](/home/theduke/dev/github.com/wasmerio/ubi/wasix/src/wasix_compat.cc) and callsites in [src/edge_process.cc](/home/theduke/dev/github.com/wasmerio/ubi/src/edge_process.cc). These currently avoid unresolved imports for `uv_get_available_memory`, `uv_get_constrained_memory`, and `uv_resident_set_memory`, but should either report real values or be explicitly feature-gated.
- Fix libuv stdio integration for WASIX instead of relying on compatibility behavior in `ubi` or the harness. Current evidence:
  - guest-side direct WASIX writes work
  - the deeper guest/host memory issue was in the N-API/V8 buffer bridge rather than Wasix stdout capture alone
  - stdio should continue to work through the normal Node/libuv path now that Buffer and typed-array sharing are correct
  The remaining cleanup is to make sure `uv_tty_*`, stdio handle classification, and any TTY-specific behavior work correctly on WASIX without any special-case relabeling or forced handle types.

- Revisit external `ArrayBuffer` / `Buffer` backing semantics in [napi/v8/src/js_native_api_v8.cc](/home/theduke/dev/github.com/wasmerio/ubi/napi/v8/src/js_native_api_v8.cc). WASIX now needs truly shared external backing stores so guest native code and host-side JS see the same bytes. The current fix moved the bridge onto real external backing stores, but this path should be reviewed carefully against upstream `napi-v8` expectations and GC/finalizer behavior.

## Build system

- Stop `ubi/wasix/setup-wasix-deps.sh` from mutating cloned deps during normal builds; switch to pinned revisions or explicit update steps.
- Reduce noisy WASIX compatibility warnings in the build once the functional gaps are resolved.
- Builtin JS loading for WASIX now uses packaged sources (`EDGE_EMBED_BUILTIN_SOURCES=OFF`):
  - builtin IDs and paths are generated at build time into a compiled catalog, similar to native Node
  - source files are staged into `<build>/node-lib/` (deps under `internal/deps/`) and read from `/node-lib`, or from `EDGEJS_LIB_DIR` when set
  - a `.edge-builtin-catalog` digest file guards against a wasm paired with a stale tree
  Builtin source loads must keep flowing through the WASIX filesystem contract rather than ad hoc host-side shortcuts.

## N-API harness

- Revisit getter/setter callback dispatch in [napi/src/napi_bridge_init.cc](/home/theduke/dev/github.com/wasmerio/ubi/napi/src/napi_bridge_init.cc). It currently distinguishes property getters from setters by callback arity when a single N-API property descriptor carries both, which is sufficient for now but should become an explicit callback-kind bridge.
- Replace the temporary no-op `napi_add_env_cleanup_hook()` / `napi_remove_env_cleanup_hook()` bridge in [napi/src/lib.rs](/home/theduke/dev/github.com/wasmerio/ubi/napi/src/lib.rs) with a real cleanup-hook registry that invokes guest callbacks during env teardown.
- Revisit the explicit `ubi_guest_malloc` export used by the WASIX harness for guest-backed `ArrayBuffer` / typed-array creation (see [wasix/src/wasix_compat.cc](/home/theduke/dev/github.com/wasmerio/ubi/wasix/src/wasix_compat.cc) and [napi/src/ctx.rs](/home/theduke/dev/github.com/wasmerio/ubi/napi/src/ctx.rs)). It unblocks correct N-API behavior today, but the long-term interface should likely become a cleaner, generic guest allocator contract rather than a `ubi`-specific exported symbol.
- Remove long-lived raw store-pointer fallback from the callback trampoline in [napi/src/guest/callback.rs](/home/theduke/dev/github.com/wasmerio/ubi/napi/src/guest/callback.rs). Current top-level callback state persists an erased store pointer to support callbacks that fire outside an active `FunctionEnvMut` stack frame (for example interrupt callbacks from [napi/src/napi_bridge_init.cc](/home/theduke/dev/github.com/wasmerio/ubi/napi/src/napi_bridge_init.cc)). Replace this with a queued/deferred callback dispatch model that stores only lightweight callback metadata and drains when a live env/store is available.
