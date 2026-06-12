# QuickJS Structured Clone / Serdes for SharedArrayBuffer

Why this is a problem:

Node worker and MessagePort internals serialize data between threads. The
QuickJS N-API serdes path must preserve `SharedArrayBuffer` backing storage and
identity semantics. Without this, worker bootstrap data can raise
`messageerror`, stall, or lose shared state.

When it occurs:

- worker smoke tests;
- webcrypto worker/shared-buffer paths;
- Node internals using `SharedArrayBuffer` counters.

Minimal manifestation:

```js
const { MessageChannel } = require('node:worker_threads');
const { port1, port2 } = new MessageChannel();

port1.on('message', (value) => {
  console.log(value instanceof SharedArrayBuffer, value.byteLength);
});

port2.postMessage(new SharedArrayBuffer(4));
```

Boundary:

```text
Node worker/message_port JS
  -> N-API serializer
  -> QuickJS JS_WriteObject2(JS_WRITE_OBJ_SAB | JS_WRITE_OBJ_REFERENCE)
  -> N-API deserializer
  -> QuickJS JS_ReadObject2(JS_READ_OBJ_SAB | JS_READ_OBJ_REFERENCE)
```

Proposed solution:

Install QuickJS shared-array-buffer functions on the runtime and use
`JS_WriteObject2()` / `JS_ReadObject2()` with SAB/reference flags in the N-API
serdes implementation.

Sketch:

```cpp
JSSharedArrayBufferFunctions funcs{};
funcs.sab_alloc = napi_shared_array_buffer__::alloc;
funcs.sab_free = napi_shared_array_buffer__::free;
funcs.sab_dup = napi_shared_array_buffer__::dup;
JS_SetSharedArrayBufferFunctions(rt, &funcs);

JSValue bytes = JS_WriteObject2(ctx, obj,
    JS_WRITE_OBJ_SAB | JS_WRITE_OBJ_REFERENCE);
JSValue value = JS_ReadObject2(ctx, data, len,
    JS_READ_OBJ_SAB | JS_READ_OBJ_REFERENCE);
```

Keep allocation in an internal helper such as:

```text
napi/quickjs/src/internal/napi_shared_array_buffer.{h,cc}
```

Use `new (std::nothrow)[]` / `delete[]` style consistent with the codebase.

## Proposed Solution References

Proposed solution can be found in:

**Reference commits:**

```text
napi 21f22e3 QJS: Serdes for SharedArrayBuffer
```

**Cross-project pointer:**

```text
edgejs 38e01f79 Napi/Qjs: SharedArrayBuffer. Ssl certs in wasix tests.
```
