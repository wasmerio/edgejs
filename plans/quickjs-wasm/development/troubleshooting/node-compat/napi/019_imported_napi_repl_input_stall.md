# Imported N-API REPL input stall

| | | Remarks |
| --- | --- | --- |
| **Status** | ▶️ | Active investigation after imported N-API startup reaches the REPL prompt but does not evaluate input. |
| **Severity** | High | Blocks interactive REPL use for the root V8/imports WASIX package under Wasmer. |

## Failure

After the imported N-API wrap/finalizer fix, the root package starts:

```sh
~/src/dev/wasmer/target/release/wasmer run --experimental-napi -l .
```

It prints:

```text
Welcome to Edge.js 0.0.0-faf7e02 (Node.js v24.13.2).
Type ".help" for more information.
>
```

Typing:

```js
console.log("hello")
```

echoes the line but does not evaluate it or print a new prompt. `Ctrl-D` also
does not exit cleanly. `Ctrl-C` terminates the host run and logs:

```text
signal handler runtime error: RuntimeError: indirect call type mismatch
```

Disabling persistent history does not make input evaluate:

```sh
NODE_REPL_HISTORY="" ~/src/dev/wasmer/target/release/wasmer run --experimental-napi -l .
```

This distinguishes the failure from the old embedded QuickJS REPL history stall,
where disabling history confirmed the promise-continuation diagnosis.

## Action Plan

1. Keep this separate from the prior `napi_wrap(...)` startup fix.
2. Trace the imported N-API callback path used when host V8 invokes a JS
   function backed by a guest WASM callback.
3. Verify whether `generic_wasm_callback(...)` has a valid callback context when
   callbacks fire from WASIX/libuv events such as TTY input and signal delivery.
4. Inspect the function table typing for `snapi_host_invoke_wasm_callback(...)`
   and the guest callback function pointers stored by `snapi_bridge_register_callback(...)`.
5. Add a narrow imported-NAPI smoke test that exercises an event-driven callback
   after startup, not just synchronous `console.log(...)`.
6. Rebuild source Wasmer with `napi-v8`, then verify:

```sh
~/src/dev/wasmer/target/release/wasmer run --experimental-napi . -- -e "setTimeout(() => console.log('timer'), 0)"
NODE_REPL_HISTORY="" ~/src/dev/wasmer/target/release/wasmer run --experimental-napi -l .
~/src/dev/wasmer/target/release/wasmer run --experimental-napi -l .
```

Expected REPL behavior: entered expressions evaluate, a new prompt appears, and
EOF exits.

## Findings

This is not the old QuickJS microtask/history issue:

- Embedded QuickJS WASIX still runs timers and immediates under Wasmer:

```sh
cd ~/src/dev/edgejs/quickjs-wasm
wasmer run . -- -e "setTimeout(() => console.log('timer'), 10); console.log('scheduled')"
wasmer run . -- -e "setImmediate(()=>console.log('immediate')); Promise.resolve().then(()=>console.log('promise'));"
```

Both commands print the expected asynchronous output.

- Imported N-API with source Wasmer drains V8 promise microtasks:

```sh
~/src/dev/wasmer/target/release/wasmer run --experimental-napi . -- -e "Promise.resolve().then(() => console.log('promise'))"
```

This prints `promise`.

- Imported N-API does not run libuv-backed work:

```sh
~/src/dev/wasmer/target/release/wasmer run --experimental-napi . -- -e "setTimeout(() => console.log('timer'), 10); console.log('scheduled')"
~/src/dev/wasmer/target/release/wasmer run --experimental-napi . -- -e "require('fs').promises.open('/tmp/edge-imports-repl-probe','a+').then(h=>h.close()).then(()=>console.log('closed'))"
```

The timer command prints only `scheduled`; the fs promise never reaches
`closed`.

- `setImmediate` is also broken in imported mode:

```sh
~/src/dev/wasmer/target/release/wasmer run --experimental-napi . -- -e "setImmediate(()=>console.log('immediate')); Promise.resolve().then(()=>console.log('promise'));"
```

It prints `promise` and then stalls.

- Direct timer binding calls are present and callable:

```sh
~/src/dev/wasmer/target/release/wasmer run --experimental-napi . -- -e "const b=internalBinding('timers'); console.log('types', typeof b.setupTimers, typeof b.scheduleTimer, typeof b.toggleTimerRef); b.scheduleTimer(10); b.toggleTimerRef(true); console.log('done')"
```

This prints `types function function function` and `done`, but no timer callback.

- A temporary Wasmer-side callback trace showed `setupTimers`, `scheduleTimer`,
  and `toggleTimerRef` are invoked with an active callback context during
  bootstrap and top-level eval. The problem is therefore below simple
  `generic_wasm_callback(...)` dispatch for synchronous native binding calls.

- Temporarily enabling `enable_blocking_sleep` while keeping N-API async
  threading disabled did not fix timers and caused broader hangs. Removing the
  N-API async-threading override entirely also caused early abnormal exit. These
  were reverted.

Current working hypothesis: imported N-API synchronous callbacks can enter the
guest, and V8 promise microtasks drain, but guest libuv/event-loop liveness or
event callback delivery is not preserved in the imported-NAPI WASIX run. The
REPL symptom is likely the same class as timers/fs/immediate: stdin/readline is
waiting on libuv work that never reaches the JS callback.

### LLDB / debug Wasmer findings

Built a debug Wasmer CLI with the same imported N-API and LLVM feature set:

```sh
cd ~/src/dev/wasmer
cargo build -p wasmer-cli --features llvm,napi-v8,wasmer-artifact-create,static-artifact-create,wasmer-artifact-load,static-artifact-load --bin wasmer
```

The debug binary reproduces the failure on the LLVM path:

```sh
~/src/dev/wasmer/target/debug/wasmer run --wasmer-dir /private/tmp/wasmer-debug-cache --disable-cache --experimental-napi -l . -- -e "console.log('sync'); Promise.resolve().then(()=>console.log('promise')); setTimeout(()=>console.log('timer'),10); console.log('scheduled')"
```

It prints `sync`, `scheduled`, and `promise`, but not `timer`.

LLDB confirms the imported N-API runner disables WASIX asynchronous threading
and enters the guest synchronously:

```text
ContextSwitchingEnvironment::run_main_context
  context_switching.rs:121 let result = entrypoint.call(&mut store, &params);
```

So Wasmer is not directly running Edge's JavaScript loop. The guest EdgeJS wasm
calls its own compiled-in libuv `uv_run(...)`; Wasmer executes that guest code,
and guest libuv crosses into native Wasmer only for WASIX syscalls such as
`poll_oneoff`. With `-l/--llvm`, the guest wasm frames are LLVM-generated native
code, so source-level breakpoints are reliable at the host WASIX/N-API import
boundary rather than inside guest EdgeJS C++.

Wasmer-side tracing and LLDB both show that the guest loop does reach WASIX
polling after `setTimeout(...)` is scheduled:

```text
poll_oneoff(..., nsubscriptions=3)
```

The `RUST_LOG=wasmer_wasix::syscalls::wasi::poll_oneoff=trace` run reports
successful clock events, but `NODE_DEBUG=timer` only logs insertion:

```text
TIMER 1: no 10 list was found in insert, creating a new one
```

It never logs `process timer lists` or `timeout callback`.

Breakpoint-hit comparison under LLDB:

```text
no timer:
  snapi_bridge_call_function: 164
  generic_wasm_callback: 218
  poll_oneoff: 9

setTimeout(..., 10):
  snapi_bridge_call_function: 171
  generic_wasm_callback: 260
  poll_oneoff: 10
```

Skipping the no-timer baseline and printing names for the seven extra
`snapi_bridge_call_function(...)` hits produced:

```text
processTicksAndRejections
emit
internalBinding
emit
emitDestroyNative
emitDestroyNative
internalBinding
```

`processTimers` does not appear. This means the timer-specific extra JS calls
are nextTick/lifecycle/cleanup work, not the JS timer dispatcher.

Current narrowed diagnosis: the guest EdgeJS event loop reaches native Wasmer
WASIX polling and receives clock readiness, but Edge's native timer callback
does not get as far as calling the JS `processTimers(now)` dispatcher. The next
breakpoint should be inside the guest Edge timer path if debug info permits, or
a temporary trace in `src/edge_environment.cc::Environment::OnTimer` /
`src/edge_timers_host.cc::CallTimersCallback` should verify whether `OnTimer`
fires, whether `can_call_into_js()` rejects the callback, whether the timers
callback reference is missing, or whether `EdgeMakeCallbackWithFlags(...)`
returns without invoking `processTimers`.

## Verification State

- Source Wasmer was rebuilt with the imported N-API finalizer ABI fix restored.
- `console.log('plain')` works under imported N-API after the first-run Wasmer
  compile/cache delay.
- `setTimeout` still prints only the synchronous line.
- The local `~/.wasmer` compiled-module cache write is blocked in the sandbox,
  so first runs after rebuilding `build-wasix/edgejs.wasm` can pause before any
  Edge bootstrap trace appears.
