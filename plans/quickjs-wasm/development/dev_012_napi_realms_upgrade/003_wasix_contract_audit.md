# N-API realms upgrade: WASIX contract audit

| Field | Value |
| --- | --- |
| Status | 🟢 Complete |
| Scope | Audit imported N-API layouts, validation tooling, build compatibility and available WASIX verification. |
| Dependencies | N-API main `18209d2`; primary worker owns native runtime migration. |
| Write ownership | Audit worker: import validation/build helpers if needed, `src/edge_runtime_platform_v8.cc`, `tests/runners/test_0_runtime_phase01.cc`, and this note. Primary owns the other runtime edits and submodules; realm-test worker owns its JS fixture and CMake registration. |
| Verification | Import-validator tests, shell syntax checks, WASIX builds/smokes coordinated with primary worker. |

Inspect the new realm exports and appended version-2 module/bytecode options.
Do not duplicate N-API's own suites in EdgeJS. Report native/guest ABI hazards,
required tooling updates, and unavailable prerequisites. Other workers may be
active; keep edits in the stated write set. Coordinate expensive builds rather
than starting one alongside primary builds.

## Foreground hook action plan

The new `unofficial_napi_enqueue_foreground_task_callback` contract transfers
task ownership on every invocation, even when rejected. Cleanup must run
exactly once after execution or refusal; an error return guarantees that the
callback cannot run. Audit found these Edge adapter violations:

- `uv_async_send` failure returns an error after the task has entered a queue,
  so a later drain can still run the refused callback.
- Delayed tasks clean up in the asynchronous timer-close callback rather than
  immediately after execution, and shutdown can clear the env first.
- A foreground batch remaining after a scope/exception failure loses its
  cleanup callbacks.
- Millisecond truncation can schedule delayed work before its requested delay.

Before validation, change only the adapter to submit after successful wakeup,
make cleanup idempotent, clean delayed task data before closing the timer,
preserve unprocessed batches, and round delay upward. Move callbacks out of
queue mutexes so cleanup reentry cannot deadlock. Extend the existing Edge
runtime C++ fixture to exercise submission, delayed cleanup and shutdown
refusal. The primary worker authorized this write-set extension.

## Foreground hook implementation and verification

Implemented in `src/edge_runtime_platform_v8.cc`:

- A failed wakeup never inserts the task in either queue, and every refusal
  releases handed-over data once.
- Timer execution cleans task data immediately after its callback, with an
  idempotent cleanup helper shared by timer closure and shutdown. A running
  timer defers cleanup until its callback returns if it triggers shutdown.
- The adapter retains its env identity for late refusals during provider
  release. Shutdown discards task data synchronously, before timer handles
  finish closing, and invokes cleanup outside the queue mutex.
- Unprocessed foreground batches survive scope/exception failures. Shutdown
  during a batch drops and cleans its remaining work.
- Delayed callbacks check a monotonic deadline. This is necessary even with
  upward delay rounding because libuv uses a cached millisecond clock.

Three new tests in `tests/runners/test_0_runtime_phase01.cc` pass against the
upgraded V8 provider: cross-thread deferred execution/exact cleanup, cleanup
before the next delayed callback, and synchronous cleanup on shutdown or late
refusal. The existing nested native-immediate test also passes. Commands:

```sh
cmake --build build-edge --target edge_test_0_runtime_phase01 -j4
build-edge/tests/edge_test_0_runtime_phase01 --gtest_filter='Test0RuntimePhase01.*Foreground*'
build-edge/tests/edge_test_0_runtime_phase01 --gtest_filter='Test0RuntimePhase01.NativeImmediateQueueRunsBeforeJsImmediatesAndDrainsNestedTasks'
```

Logs: `/private/tmp/edge-realms-foreground-build.log` and
`/private/tmp/edge-realms-foreground-tests.log`. `git diff --check` passes.

## Initial audit results

- `make test-wasix-import-validator validate-wasix-imports` passes (8 validator
  tests; the **old** guest has 110 standard/67 extension imports). This confirms
  inventory compatibility only, not that the stale artifact uses realms.
- Both native and host-JS registries export create/get/enter/leave realm,
  `run_script`, and `compile_function`; the existing inventory parser already
  recognizes these names and needs no layout-specific update.
- Module/bytecode options are already zero-initialized in Edge. Version 2 adds
  one realm pointer at the end (wasm32 module options 40→44 bytes; bytecode
  options 48→52 bytes); env hooks remain version 1/32 bytes. The new header has
  wasm32 size assertions for all of these.
- Installed tools: arm64 host, wasmer 7.5.0, wasixcc 0.4.7. WASIX artifacts
  currently date from October 2 and must be rebuilt sequentially after source
  migration. `bash -n wasix/build-wasix.sh quickjs-wasm/build.sh` passes.
- A compile-only wasm32 C++ probe with wasixcc succeeds, checking the public
  header's ABI assertions plus the module/bytecode version constants and
  64-bit realm-scope token. The wasixcc wrapper does not honor `-fsyntax-only`
  without attempting a final link; use `-c -o` for this probe.
- `build-wasix-napi/target/release/napi_wasmer` dates from August 31. Rebuild
  the standalone runner from the new N-API pin before claiming imported-guest
  runtime compatibility. Names-only validation cannot establish host ABI or
  struct-version support in an already installed Wasmer binary.

## QuickJS WASIX verification

The required rebuild form was used from `quickjs-wasm/`:

```sh
./build.sh > /private/tmp/edge-realms-wasix-build.log 2>&1
```

Build, no-N-API-imports/non-WASI-import gate, and pnpm staging pass. The
following also pass under installed Wasmer 7.5.0:

```sh
wasmer package build --check ./quickjs-wasm
wasmer run ./quickjs-wasm -- --version
wasmer run ./quickjs-wasm -- -e "console.log('hello from quickjs')"
wasmer run --volume /Users/syrusakbary/Development/edgejs/tests:/tests ./quickjs-wasm -- --experimental-vm-modules /tests/js/napi-realms.js
python3 scripts/test-wasix-safe-mode.py --package-dir quickjs-wasm
```

The version is `v24.13.2-pre` and the realm fixture reports `NAPI_REALMS_OK`.
Its VM Modules experimental warning is expected. The seven deterministic
safe-mode checks pass; outbound network checks were not run. Safe-mode log:
`/private/tmp/edge-realms-wasix-smoke.log`. The first version run emitted
Wasmer's cold-cache missing-file-removal warning; its sequential repeat is
clean. The existing user edit to the package version remains untouched.

Review of the provider sandbox-sync fix found that source-root synchronization
must exclude standalone synthetic modules and account for nested synthetic
callbacks in source graphs. The realm-test worker's provider correction and
regressions are documented in [002_compatibility_tests.md](002_compatibility_tests.md).
After that correction, a final incremental `quickjs-wasm/ ./build.sh`, package
check, updated realm fixture, and all seven deterministic safe-mode checks
pass. The updated fixture covers standalone synthetic success/rejection
sandbox writes, source graph writes, and nested synthetic evaluation.

Intermediate build/smoke logs are `/private/tmp/edge-realms-wasix-final-build.log` and
`/private/tmp/edge-realms-wasix-final-smoke.log`. Intermediate embedded QuickJS WASIX
SHA-256:

```text
3ea217955f867bd2dd803ea03588aa8cc01db37ad34a944b3bacec16ff5517b3
```

Imported-WASIX and standalone-runner builds remain owned and recorded by the
primary worker. This audit's imported-artifact result above is explicitly for
the old binary; it must not be read as verification of the migrated guest.

## Final integration verification

After the final shared import-meta/synthetic callback helper, the primary
worker rebuilt embedded QuickJS WASIX again and verified the complete public
realm fixture, package check and seven deterministic safe-mode smoke cases.
The first cold Wasmer cache lookup emits a cache-file warning; a warm rerun
passes the smoke suite's strict stderr checks. Final logs use the
`/private/tmp/edge-realms-wasix-complete-*` prefix (`smoke-warm.log` for smoke).
The final embedded artifact SHA-256 is:

```text
5b0f9071830f15cfb0af5af65be8f494798b2fa87fedbcf0f1fe00d4c4359302
```

The primary worker also rebuilt the imported guest and the locked standalone
runner. The final engine-free import gate passes with 110 N-API and 68
extension imports, both package checks pass, and the complete realm fixture
passes through the new imported ABI. Issue 023's reentry guards remove both
cache-buffer dropped-callback diagnostics. Imported logs:
`/private/tmp/edge-realms-imported-wasix-complete-{build,realms}.log`. Final
imported artifact SHA-256:

```text
79530344bda51edec40ebb255cc36030daf9803a6472feacefc657bc61c8aa26
```
