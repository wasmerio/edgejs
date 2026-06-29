# Imported N-API wrap finalizer bridge

| | | Remarks |
| --- | --- | --- |
| **Status** | 🟠 | Startup fix works with rebuilt source Wasmer; signal delivery and true guest finalizer invocation remain follow-up work. |
| **Severity** | High | Blocks the imported N-API root package before the REPL can start. |

## Failure

Running the root V8/imports WASIX package with installed Wasmer reaches Node
bootstrap and fails when the REPL logs its greeting:

```sh
wasmer run --experimental-napi -l .
```

Observed failure:

```text
TypeError: Illegal invocation
    at process.startListeningIfSignal (node:internal/process/signal:28:10)
    at process.getStdout [as stdout] (node:internal/bootstrap/switches/is_main_thread:159:13)
    at console.log (node:internal/console/constructor:416:26)
    at node:internal/main/repl:38:13
```

Running the source Wasmer binary from `~/src/dev/wasmer` currently fails earlier:

```text
Error while importing "napi_extension_wasmer_v0"."unofficial_napi_create_env": unknown import.
```

That binary accepts `--experimental-napi`, but the observed behavior is
consistent with a build without the `napi-v8` feature, so it never installs the
N-API runtime hook that provides the extension imports.

## Diagnosis

The JS failure is in Edge's `SignalWrap` bootstrap path:

1. `process.stdout` detects TTY output and registers `SIGWINCH`.
2. `internalBinding('signal_wrap').Signal` constructs a native `SignalWrap`.
3. `SignalCtor(...)` calls `napi_wrap(env, self, wrap, SignalFinalize, ..., &wrapper_ref)`.
4. The imported N-API bridge receives the non-null guest finalizer pointer but
   discards it.
5. `snapi_bridge_wrap(...)` calls host V8 `napi_wrap(...)` with
   `finalize_cb == nullptr` and `result != nullptr`.
6. The V8 backend rejects that combination with `napi_invalid_arg`.
7. `SignalCtor(...)` currently does not check the status, so the later
   `SignalStart(...)` cannot unwrap `this` and throws `Illegal invocation`.

The bridge also drops guest finalizers for `napi_add_finalizer(...)`, which is a
related lifecycle compatibility gap.

## Action Plan

1. Keep the fix in the imported N-API bridge, not in `SignalWrap`.
2. Preserve the existing guest ABI shape for ordinary wraps.
3. Teach the bridge whether the guest supplied a finalizer.
4. Use a host-side no-op finalizer shim when the guest supplied one, so V8's
   `napi_wrap(..., result)` precondition is satisfied.
5. Rebuild the source Wasmer CLI with the `napi-v8` feature.
6. Verify:

```sh
wasix/build-wasix.sh
wasmer run --experimental-napi -l .
wasmer run --experimental-napi . -- -e "console.log('hello from imports')"
```

For parity with the source Wasmer checkout, rebuild Wasmer with the `napi-v8`
feature before comparing runtime behavior.

## Fix

Implemented in the imported N-API bridge:

- `guest_napi_wrap(...)` and `guest_napi_add_finalizer(...)` now pass a
  `has_finalize_cb` flag to the host bridge.
- `snapi_bridge_wrap(...)` supplies `NoopGuestFinalizer` when the guest provided
  a finalizer pointer, which satisfies the V8 backend rule that
  `napi_wrap(..., result)` requires a non-null finalizer.
- `snapi_bridge_add_finalizer(...)` uses the same no-op host finalizer when the
  guest supplied a finalizer pointer.

This preserves imported object wrapping and unwrapping for native Edge wrapper
classes such as `SignalWrap`. It does not yet invoke the guest's actual
finalizer callback.

## Verification

Direct C++ syntax verification passed against the Wasmer checkout's generated
V8 include directory:

```sh
c++ -std=c++20 -fsyntax-only -DNAPI_EXTERN= -DV8_COMPRESS_POINTERS=1 \
  -I ~/src/dev/wasmer/target/debug/build/wasmer-napi-4a90baddef866940/out/v8-prebuilt/11.9.2/darwin-arm64/include \
  -I ~/src/dev/wasmer/lib/napi/include \
  -I ~/src/dev/wasmer/lib/napi/lib/src \
  -I ~/src/dev/wasmer/lib/napi/v8/src \
  ~/src/dev/wasmer/lib/napi/src/napi_bridge_init.cc
```

The check emitted only the existing `napi_float16_array` switch warning.

The source Wasmer CLI was rebuilt with N-API enabled:

```sh
cargo build --release -p wasmer-cli \
  --features llvm,napi-v8,wasmer-artifact-create,static-artifact-create,wasmer-artifact-load,static-artifact-load \
  --bin wasmer
```

The rebuilt binary passed the imported N-API smoke command:

```sh
~/src/dev/wasmer/target/release/wasmer run --experimental-napi . -- -e "console.log('hello from imports')"
```

Output:

```text
hello from imports
```

The original REPL startup command now reaches the prompt:

```sh
~/src/dev/wasmer/target/release/wasmer run --experimental-napi -l .
```

Output reached:

```text
Welcome to Edge.js 0.0.0-faf7e02 (Node.js v24.13.2).
Type ".help" for more information.
>
```

## Remaining Caveats

Sending Ctrl-C to the REPL after startup produced a separate signal callback
runtime error:

```text
signal handler runtime error: RuntimeError: indirect call type mismatch
```

Treat that as a follow-up signal callback/trampoline issue. It is distinct from
the startup `Illegal invocation`, which was caused by failed `napi_wrap(...)`.
