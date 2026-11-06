# Async store callbacks need nonblocking acquisition on current Wasmer

| | | Remarks |
| --- | --- | --- |
| **Status** | 🟢 | Resolved by the published Wasmer API addition and N-API adapter. |
| **Severity** | Medium | Native HTTP works; cancelling a queued host-JS store lock can retain waiter slots and prevent teardown. |

Wasmer main `4434ce0` removes `AsyncFunctionEnvMut::try_write` and
`with_current_mut`. The N-API JavaScript host needs synchronous callback entry
that defers immediately when its JSPI store is busy. Polling public
`AsStoreAsync::write_lock()` once reproduces a teardown failure: cancellation
leaves an empty slot in `LocalRwLock.write_waiters`, and `consume()` requires
the vector itself to be empty. Repeated deferrals also accumulate slots.

Action plan before changing Wasmer: expose nonblocking async-store acquisition
using its existing `LocalRwLock::try_write`, with the same safe guard type as
`write_lock()`. Add ownership/contention regression coverage, adapt N-API to
that API, and rerun the focused wasm tests plus host-context and callback
liveness suites. Preserve deferral semantics without a background continuation
that retains the store. Publish the upstream fix and pin the N-API standalone
dependency to that descendant of current Wasmer main.

Ownership: compatibility worker owns the isolated Wasmer API change and
N-API `src/guest/callback_js.rs` / `napi_js.rs`; primary owns dependency pins,
lockfiles, CLI/compiler selection and Edge documentation. Native benchmark
timings proceed on immutable `4434ce0`; pause builds during the timed windows.

See [the benchmark note](../../../dev_012_napi_realms_upgrade/005_http_throughput_benchmark.md).

## Resolution and validation

Wasmer `a021148d9829695134936a2524c2bf0a8306bc34` adds
`AsStoreAsync::try_write_lock()`, returning an owning guard only when the
store is immediately available and registering no waiters. N-API
`4179e538007b12b4b28cd8f1bc521990e09e0a3b` uses that API for persistent
callbacks, recovers synchronous stores only from explicit parking, and uses
public current memory-buffer accessors. Callback handles stay alive during
guest reentry even if the guest releases its environment.

Companion: [wasmerio/wasmer#7143](https://github.com/wasmerio/wasmer/pull/7143),
required by [wasmerio/napi#140](https://github.com/wasmerio/napi/pull/140).

- Wasmer API ownership/contention tests: native 6/6 and wasm 2/2 pass.
- Wasmer `make lint` and required CLI build: pass.
- N-API wasm library: 40/40 pass, including three new regressions for store
  identity/nested borrowing, busy-store deferral/teardown, and shared/nonshared
  memory growth. The teardown assertion failed with cancelled-future polling
  and passes with the new nonblocking API.
- Node host-context tests: 41/41 pass.
- Final native N-API tests: 90 pass, one existing ignored test.
- Final LLVM CLI build and Edge public realm fixture: pass.

This avoids cancellation entirely; the general upstream cancelled-waiter
compaction behavior is outside this fix. Logs are under
`/private/tmp/edge-napi-host-js-latest-*.log` and
`/private/tmp/edgejs-http-llvm-tools/*-final.log`.
