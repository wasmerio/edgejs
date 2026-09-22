# Node Test: HTTP/2 native lifecycle crashes

| | | Remarks |
| --- | --- | --- |
| **Status** | ▶️ | Reopened: recurring native V8 Linux/macOS shutdown signals remain; a native crash stack is required before another logic change. |
| **Severity** | High | Signal 5/7/10/11 crashes intermittently block native V8 CI. |

## Symptoms

QuickJS crashed with SIGBUS/SIGSEGV in HTTP/2 tests such as:

```text
test/parallel/test-http2-socket-proxy-handler-for-has.js
test/sequential/test-http2-timeout-large-write.js
test/sequential/test-http2-timeout-large-write-file.js
```

LLDB showed the crash under `WriteChunksToParent(...)` /
`FlushSessionOutput(...)`, after HTTP/2 attempted to write pending session bytes
through the parent stream.

## Root Cause

`SessionConsume()` stored the result of `EdgeStreamBaseFromValue(socket._handle)`
as the HTTP/2 parent stream. QuickJS N-API constructed native class instances
with the same QuickJS class used for `napi_create_external(...)`, so
`napi_typeof()` reported wrapped objects such as `TLSWrap` as `napi_external`.
The generic stream extraction path then accepted the wrapped native object
pointer directly.

For TLS sockets that meant HTTP/2 sometimes stored a `TlsWrap*` as if it were an
`EdgeStreamBase*`. Later writes read the stream ops table from the wrong offset
and jumped through heap data.

## Fix

The fix stays outside `lib/` and keeps the EdgeJS shared native stream code
unchanged:

- QuickJS N-API constructor calls now create ordinary prototype-backed objects,
  not objects of the external class.
- `napi_typeof()` reports `napi_external` only for actual values created by
  `napi_create_external(...)`.
- `napi_get_value_external()` rejects non-external values instead of returning a
  wrapped object's native pointer.

## Verification

Targeted QuickJS checks passed after rebuilding `build-edge-quickjs-cli/edge`:

```sh
build-edge-quickjs-cli/edge test/parallel/test-http2-socket-proxy-handler-for-has.js
build-edge-quickjs-cli/edge test/sequential/test-http2-timeout-large-write.js
build-edge-quickjs-cli/edge test/sequential/test-http2-timeout-large-write-file.js
build-edge-quickjs-cli/edge test/parallel/test-http2-too-many-settings.js
build-edge-quickjs-cli/edge test/parallel/test-http2-multiplex.js
build-edge-quickjs-cli/edge test/parallel/test-http2-forget-closed-streams.js
```

Shared native regression checks passed after rebuilding `build-edge/edge`:

```sh
build-edge/edge test/parallel/test-http2-socket-proxy-handler-for-has.js
build-edge/edge test/sequential/test-http2-timeout-large-write.js
build-edge/edge test/parallel/test-buffer-constants.js
```

## 2026-05-16 QuickJS macOS CI Follow-up

The `test and build / quickjs-macos` GitHub Actions log
`test-failures-qjs-2.log` shows four HTTP/2 failures in the QuickJS runtime
lane:

```text
test/parallel/test-http2-client-set-priority.js       -> Signal 11
test/parallel/test-http2-compat-serverresponse-writehead.js -> Signal 11
test/parallel/test-http2-response-splitting.js        -> response splitting assertion
test/parallel/test-http2-reset-flood.js               -> timeout
```

These are not the same three tests that validated the previous
`TLSWrap*`/`EdgeStreamBase*` object classification fix. Treat this as an open
HTTP/2 follow-up and reproduce the two crashing tests under LLDB before editing
native code. Useful source areas to inspect first are the QuickJS session output
path and stream reset/priority handling in
`src/internal_binding/binding_http2.cc`.

## 2026-05-17 V8 Fix Regression Follow-up

After native fixes for the Linux/V8 JSStream destroyed-write and TLS
abort-controller late-connect tests, the QuickJS suite regressed from
17 failures to 20 failures. The new failures were:

```text
test/parallel/test-http2-close-while-writing.js
test/parallel/test-http2-create-client-connect.js
test/parallel/test-tls-close-notify.js
```

The fixes stayed native-only; no `lib/` JavaScript files were changed.

Findings:

- `test-http2-close-while-writing.js` first failed with `write ECANCELED` from
  `RunDeferredStreamDestroy(...)`. A broad HTTP/2 parent `isClosing()` guard
  converted normal close ordering into `UV_EPIPE` before queued HTTP/2 data
  could flush. Removing that guard was necessary but not sufficient.
- Node's `Http2Session::ClearOutgoing()` completes stream write wraps with
  status `0` once nghttp2 has serialized them to the parent socket write.
  Edge was propagating parent write errors back to stream writes. HTTP/2 now
  completes those serialized stream writes with `0` and forwards `0` for the
  internal parent write request.
- TLS had the same internal parent-write exposure: `ParentStreamOnAfterWrite()`
  passed `UV_ECANCELED` down the listener chain before checking whether the
  write was TLS-owned. TLS now forwards status `0` for its own internal parent
  write request while preserving the real status for TLS bookkeeping.
- For no-error or `NGHTTP2_CANCEL` stream teardown, deferred HTTP/2 destroy now
  completes remaining queued stream writes with `0`; reset/error-code teardown
  still uses `UV_ECANCELED`.
- `test-http2-create-client-connect.js` then crashed during teardown GC:
  `TlsWrapFinalize()` called `NotifyTlsStreamClosed()` after environment
  cleanup had started, reaching HTTP/2 listener state that was already being
  finalized. TLS finalization now skips JS-facing close notification when
  `Environment::cleanup_started()` is true.
- `EdgeStreamNotifyClosed()` also now saves the next listener before invoking
  `on_close`, allowing close callbacks to remove or mutate listener links
  safely.

Verification:

```sh
cmake --build build-edge-quickjs-cli --target edge -j4
python3 test/tools/test.py --timeout 30 --test-root ./test \
  --shell ./build-edge-quickjs-cli/edge -j 1 \
  parallel/test-http2-close-while-writing \
  parallel/test-http2-create-client-connect \
  parallel/test-tls-close-notify
```

Result: `+3 -0`.

## 2026-05-17 TCP connect correction

The first TLS abort-controller pass added a TCP-side owner-state heuristic that
called into JS from `OnConnectDone(...)`, read the TCP handle's `owner_symbol`,
and treated a TLS owner with `encrypted === true`, `connecting === true`, and
`readable === false` as an abort. Source comparison with Node showed this was
the wrong layer: Node's `ConnectionWrap::AfterConnect()` passes libuv's status
through unchanged and only derives the readable/writable booleans from
`uv_is_readable()` / `uv_is_writable()` for successful connects.

That heuristic was removed. The TCP connect callback now follows Node's native
contract and no longer calls `internalBinding()`, clears pending exceptions, or
inspects TLS JS properties from native TCP code.

Verification after removing the heuristic:

```sh
cmake --build build-edge-quickjs-cli --target edge -j4
python3 test/tools/test.py --timeout 30 --test-root ./test \
  --shell ./build-edge-quickjs-cli/edge -j 1 \
  parallel/test-http2-close-while-writing \
  parallel/test-http2-create-client-connect \
  parallel/test-tls-close-notify \
  parallel/test-tls-connect-abort-controller \
  parallel/test-http2-client-jsstream-destroy

make test-quickjs-only TEST_JOBS=4
```

Focused result: `+5 -0`.

Full QuickJS result returned to the earlier baseline:

```text
[06:49|% 100|+ 1757|-  17]: Done
```

The three regression-only failures were absent from the final failed-test list.

## 2026-05-19 Debug `test-http2-status-code` Handle Scope Assertion

Debug QuickJS builds reproduced another allocator assertion in:

```text
test/parallel/test-http2-status-code.js
Assertion failed: (!block->is_free(slot)), function unsafe_owner, file napi_allocator.h
```

Sandboxed reproduction stops earlier with `listen EPERM`, so the focused repro
must run outside the sandbox or in the normal local terminal.

LLDB showed a stale local `napi_value`, not a stale `napi_ref`:

```text
napi_allocator__<napi_value__, napi_scope__>::unsafe_owner(...)
  napi_env__::value_from_handle(...)
  napi_typeof(...)
  InvokeStreamCloseCallback(...)
  OnStreamClose(...)
  nghttp2_session_mem_send(...)
  FlushSessionOutput(...)
  RunDeferredSessionFlushTask(...)
```

Root cause: `CallCallbackRef(...)` opens an `edge::HandleScope`, calls
`EdgeAsyncWrapMakeCallback(...)`, stores the callback return value in an output
`napi_value`, then returns to its caller after the helper scope has closed.
`InvokeStreamCloseCallback(...)` then calls `napi_typeof(...)` on that returned
local handle.

Action plan before code changes:

1. Keep HTTP/2 native callback dispatch scoped.
2. When `CallCallbackRef(...)` or `CallCallbackRefWithResource(...)` has an
   output result, use an escapable scope and escape exactly that result to the
   caller's outer scope.
3. Leave no-result callback dispatch on plain `edge::HandleScope`.
4. Rebuild the QuickJS Edge CLI and rerun `test-http2-status-code` outside the
   sandbox, plus the earlier `test-eventemitter-asyncresource` crash repro.

Implemented in `src/internal_binding/binding_http2.cc`: HTTP/2 callback
dispatch now uses `edge::EscapableHandleScope` and only escapes the callback
return value when the caller supplied a result out-parameter.

Verification:

```sh
cmake --build build-edge-quickjs-cli --target edge -j4
build-edge-quickjs-cli/edge test/parallel/test-http2-status-code.js
build-edge-quickjs-cli/edge test/parallel/test-eventemitter-asyncresource.js
python3 test/tools/test.py --timeout 30 --test-root ./test \
  --shell ./build-edge-quickjs-cli/edge -j 1 \
  parallel/test-http2-status-code \
  parallel/test-http2-close-while-writing \
  parallel/test-http2-create-client-connect \
  parallel/test-http2-client-jsstream-destroy
```

The focused HTTP/2 cluster passed `+4 -0`.

## 2026-06-26 QuickJS native CI crash follow-up

QuickJS native CI on `ci/reenable-native` failed intermittently with Signal 11 in:

```text
test/parallel/test-http2-session-unref.js
test/parallel/test-tls-alert-handling.js
```

LLDB on `test-http2-session-unref.js` showed the crash in
`EmitAfterShutdownFrom(...)` while opening a handle scope with a stale
`listener->env` after HTTP/2 session teardown and JSStream `doClose()` deferred
`finishShutdown()` calls.

Fixes in shared native stream code:

- Save the next listener before invoking shutdown/write/wants-write callbacks
  (same pattern as the existing `EdgeStreamNotifyClosed()` fix).
- Route JSStream `finishShutdown()` through the live N-API callback `env`
  instead of stale listener env pointers.
- Skip stream listener callbacks when `Environment::can_call_into_js()` is false.
- Add HTTP/2 `ParentStreamOnAfterShutdown()` pass-through and clear parent
  listener callbacks during `Http2SessionFinalize()`.

Verification:

```sh
cmake --build build-edge-quickjs-cli --target edge -j4
for i in $(seq 1 200); do
  ./build-edge-quickjs-cli/edge test/parallel/test-http2-session-unref.js || break
  ./build-edge-quickjs-cli/edge test/parallel/test-tls-alert-handling.js || break
done
TEST_PARALLEL=1 NODE_SKIP_FLAG_CHECK=true python3 test/tools/test.py \
  --timeout 30 --test-root ./test --shell ./build-edge-quickjs-cli/edge -j 1 \
  parallel/test-http2-session-unref parallel/test-tls-alert-handling
```

Result: 200/200 direct runs and 100/100 harness runs for each test without SIGSEGV.

## 2026-08-28 recurring V8 macOS CI signals

V8 macOS CI run `33222844238`, job `99020376278`, failed with Signal 11 in:

```text
test/parallel/test-buffer-bytelength.js
test/parallel/test-http2-large-write-multiple-requests.js
```

The HTTP/2 test printed all 100 expected send and receive completions before the
process crashed, placing that occurrence in process shutdown rather than the
request/response body of the test.

Reviewing the latest 40 V8 workflow runs found 11 macOS signal failures in a
small, recurring cluster:

| Test | Signal failures |
| --- | ---: |
| `test-stream-pipeline-http2` | 5 |
| `test-buffer-bytelength` | 3 |
| `test-http2-large-write-multiple-requests` | 2 |
| `test-stream-readable-to-web` | 1 |

The observed signals vary between SIGTRAP (5), SIGBUS (10), and SIGSEGV (11).
Adjacent commits and a fresh rerun of the current branch pass. This distribution
does not look like a deterministic assertion or a test-specific semantic
failure. It is consistent with a low-probability native lifetime race or stale
pointer during stream, environment, or V8 isolate teardown, but the exact owner
cannot be established without a macOS crash report or LLDB stack.

The macOS CI and local build use the same prebuilt V8 11.9.2 archive. The
extracted local library and a fresh download of the official darwin-arm64 asset
both have SHA-256:

```text
2860c8751fccfcec81410705bbbae8a204937f3a31d3ef0cecdf905473622249
```

The hosted runner has substantially less CPU and memory than the local host,
and the exact `make test-only TEST_JOBS=4` suite takes about 3:21 in CI versus
about 2:30 locally. The runner is therefore likely exposing the race through a
different shutdown schedule; it is not using a different V8 binary.

Stress verification on the current branch remained clean:

- 1,500 mixed four-way process runs across the three most frequent tests.
- 900 more mixed runs while saturating 14 local logical CPUs.
- 1,200 more mixed runs with `MallocScribble=1` and
  `MallocPreScribble=1`.
- Five exact `make test-only TEST_JOBS=4` suites: 8,745 tests total.
- A fresh V8 macOS CI job passed all tests.

No runtime workaround or broad listener/store guard should be added from this
evidence alone. The next diagnostic change should preserve macOS
`DiagnosticReports` (`edge*.crash` and `edge*.ips`) as CI artifacts whenever
the Edge test step fails. If the report is absent, rerun only the reported test
under LLDB in batch mode and upload its backtrace. Use the resulting native
stack to identify the exact teardown owner before modifying logic; do not mask
the issue with test skips or automatic retries.

## 2026-09-22 repeated Linux Buffer SIGBUS

QuickJS upstream integration PR #154 reproduced a V8 Linux Signal 7 in
`test-buffer-bytelength.js` on runs `35688862492` and `35693874264`.
Each finished with 1,748 passes and one crash. The earlier run passed a manual
failed-job rerun, and intervening run `35692198613` passed without a rerun.
The V8 subtree and Edge runtime source are identical to the baseline, so the
QuickJS sync does not explain this defect. The user explicitly flagged its
recurrence; a green retry alone is not a resolution.

A fresh macOS V8 build passed 100 mixed Buffer/HTTP2 executions. The successful
Linux CI binary from `35692198613` then passed 100 exact Buffer test executions
in Ubuntu 24.04 AMD64 Docker (one serial and 99 with four processes, bytecode
cache disabled). That uses emulation on an ARM64 host and the successful binary,
not the failed job's binary, so it does not reproduce hosted Linux scheduling.

Read-only review found a separate concrete ownership mismatch worth diagnosing:

- N-API `ForegroundTaskRunner::PostTaskCommon` initializes the record's isolate
  state and increments pending work after the host enqueue publishes the task.
  The host can already have run cleanup by that point.
- Edge's enqueue failure paths can invoke the record cleanup, but the N-API
  caller then retrieves the task from the record and deletes the record.
- A failed `uv_async_send` reports failure after the task is already queued,
  making ownership on failure ambiguous.

These paths predate the integration and are not yet proven to cause the observed
SIGBUS. The cross-context ArrayBuffer itself is correctly escaped and its
context retained; its byte length is read through the JS getter, not a raw
N-API backing-store pointer.

Action plan before changing runtime behavior:

1. Run the existing exact test under LLDB with a separate standard ASAN build
   of Edge/N-API. Keep the prebuilt V8 engine unchanged and preserve the original
   user checkout. Record any actual native stack or sanitizer report.
2. Review every callback consumer and establish an explicit ownership contract
   for accepted and rejected foreground work, including delayed tasks and
   shutdown. Do not infer ownership solely from a return code after publication.
3. If local runs remain clean, preserve native crash diagnostics in CI so the
   hosted failure can identify its owner. Do not disable tests or add automatic
   retries to make the suite green.
4. Validate any demonstrated correction against the exact Buffer test, relevant
   N-API/platform tests, the Edge V8 suite, and the full dependent PR matrices.

Local instrumented diagnostic attempts were blocked by automatic review, so
they are not claimed as verification. Linux CI now enables core dumps for the
existing runtime test step and retains line tables in the optimized build.
On failure, LLDB writes symbolic thread backtraces and loaded-image information
from the actual process core. Only text reports are uploaded; raw process-memory
core files are removed on the runner. The original test failure remains a
failure, with no automatic test retry or changed expectation.
