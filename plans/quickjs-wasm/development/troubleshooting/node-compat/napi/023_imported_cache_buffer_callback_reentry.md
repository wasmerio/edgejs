# Imported cache serialization callback reentry

| | | Remarks |
| --- | --- | --- |
| **Status** | 🟢 | Both import guards fixed; focused Rust tests and rebuilt imported runtime confirmation pass. |
| **Severity** | Medium | Cache serialization skips a guest Buffer callback; fallback data masks the dropped callback. |

## Reproduction and LLDB diagnosis

The rebuilt imported-WASIX Edge guest through the rebuilt standalone
`napi_wasmer` runner prints `[callback trampoline] no store lent to this thread`
twice during `tests/js/napi-realms.js`, even though it reaches NAPI_REALMS_OK.
The plain hello-world import smoke is clean.

LLDB breakpoint on `std::io::stdio::_eprint` identifies both call paths:

- `guest_unofficial_napi_bytecode_serialize` -> bridge ->
  `CreateNodeBufferFromBytes` -> guest callback;
- `guest_unofficial_napi_module_wrap_create_cached_data` -> bridge ->
  `CreateNodeBufferFromBytes` -> guest callback.

V8 invokes the Edge-installed Buffer constructor/from function while packaging
cache bytes, so these imports can synchronously reenter the guest. Their
current unguarded bridge calls do not park/lend the store. Log:
`/private/tmp/edge-realms-imported-lldb.log`.

## Action plan and ownership

N-API bridge worker owns `napi/src/guest/napi.rs`, a focused N-API-owned guest
regression and this note. Use the existing `with_cb_context` guard for the two
cache-producing imports (and audit equivalent byte-buffer serialization calls).
Preserve guest exit propagation and output-pointer writes; do not broaden
guarding unrelated imports. Rebuild the standalone runner and repeat the exact
realm fixture, requiring no dropped-callback diagnostics. Other workers own the
QuickJS module wrapper, Edge runtime and test harness.

The focused regression will live in N-API's
`tests/guest_cache_callbacks.rs`: a real Wasmer WAT guest installs its own
`Buffer.from` callback, then calls both cache-producing imports from exported
guest functions. Assert that each callback runs, returns its argument through
N-API, propagates a pending exception, and re-raises a guest exit as a
`WasiError::Exit` trap without writing the output slot. Use the existing
standalone Cargo release target cache; do not add Edge root CI/Makefile jobs.

## Implementation and focused verification

Both cache-producing imports now run their bridge call inside
`with_cb_context` and return `Result<i32, WasiError>`. The `?` propagates a
nested guest exit before either output pointer is written. Normal N-API status
returns and successful output writes keep their existing behavior. No ABI
signature change is visible to the guest.

The real Wasmer guest regression passes all three cases for **both** imports:

- `Buffer.from` executes guest code and returns the original view through
  `napi_get_cb_info`;
- a callback exception returns `napi_pending_exception` and preserves the
  caller's output slot;
- a callback exit reappears as `WasiError::Exit(37)`, preserves the output
  slot, and leaves callback lending/exit state usable for the next host call.

Command, run from `napi/`:

```sh
CARGO_TARGET_DIR=/Users/syrusakbary/Development/edgejs/build-wasix-napi/target ./cargo-standalone.sh test --locked --release --features cli --test guest_cache_callbacks -- --nocapture
```

Result: **3 passed**, no failures. Log:
`/private/tmp/edge-realms-imported-cache-tests.log`.

Audit of `CreateNodeBufferFromBytes` finds only these two public cache APIs
(the module API has source and synthetic branches). `Serializer.releaseBuffer`
is a JS method reached through the existing guarded script/function-call
imports; creating the serdes binding itself does not call guest code. Other
serialization imports do not share the cache-buffer callback path. The change
therefore adds guards only to the two proven native imports; host-JS and
unrelated imports are untouched.

The primary worker rebuilt the imported WASIX guest and standalone runner,
then repeated the exact `tests/js/napi-realms.js` fixture. It reports
`NAPI_REALMS_OK` with only the expected VM Modules warning. Both the
`no store lent to this thread` diagnostic and other dropped-callback
diagnostics are absent. Log:
`/private/tmp/edge-realms-imported-wasix-complete-realms.log`. Both package
checks pass. This confirms the real Edge-installed Buffer callback path as
well as the focused guest regression. `git diff --check` passes.
