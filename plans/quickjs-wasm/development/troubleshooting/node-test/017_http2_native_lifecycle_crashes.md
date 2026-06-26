# Node Test: HTTP/2 native lifecycle crashes

| | | Remarks |
| --- | --- | --- |
| **Status** | 🟠 | Original QuickJS N-API object/external crash fixed; native shutdown teardown SIGSEGV fixed for CI crash tests. |
| **Severity** | High | Signal 10/11 crashes affected a large HTTP/2 test cluster and could terminate the QuickJS CLI. |

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
