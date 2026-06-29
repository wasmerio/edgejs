# N-API QuickJS: structured clone serdes for SharedArrayBuffer

Why this is a problem:

Node workers and MessagePorts rely on structured clone. With V8, a
`SharedArrayBuffer` remains a `SharedArrayBuffer` after serialization and
deserialization, preserving shared memory identity for typed-array views. The
QuickJS N-API serializer cannot silently deserialize it as a plain `ArrayBuffer`
or drop it: Node internals use SharedArrayBuffer-backed counters during worker
bootstrap, and crypto/webcrypto worker tests depend on that shared state.

This occurs when worker bootstrap data, MessagePort payloads, or crypto worker
messages carry `SharedArrayBuffer` or typed arrays backed by it.

Minimal Example:

```js
const assert = require('node:assert');
const { MessageChannel } = require('node:worker_threads');

const { port1, port2 } = new MessageChannel();
const sab = new SharedArrayBuffer(4);
new Uint32Array(sab)[0] = 42;

port2.on('message', (value) => {
  assert(value instanceof SharedArrayBuffer);
  assert.strictEqual(new Uint32Array(value)[0], 42);
});
port1.postMessage(sab);
```

Representative failing tests:

```text
parallel/test-webcrypto-wrap-unwrap
parallel/test-crypto-key-objects-messageport
parallel/test-webcrypto-cryptokey-workers
worker bootstrap smoke using simple-echo-threads.cjs
```

Callgraph and boundary:

Current problematic path:

```text
JavaScript port.postMessage(sharedArrayBuffer)
  -> Node worker/message_port layer
  -> QuickJS N-API serializer in napi_serdes.cc
  -> JS_WriteObject()/JS_ReadObject()
     HERE IS THE PROBLEM: the basic QuickJS object writer does not carry the
     SharedArrayBuffer table needed to reconstruct SAB values as SAB values.
  -> receiver gets messageerror, wrong type, or non-shared backing storage
```

The boundary is N-API <-> QuickJS. EdgeJS should not inspect or rewrite worker
payloads; the JS engine integration must implement the structured-clone contract.

Proposed solution:

Install QuickJS `JSSharedArrayBufferFunctions` for allocation/refcounting and
use `JS_WriteObject2()` / `JS_ReadObject2()` with a `JSSABTab` when serializing
and deserializing values that may contain SharedArrayBuffer.

Relevant N-API / QuickJS code paths:

```text
~/src/edgejs/napi/quickjs/src/internal/napi_serdes.cc
~/src/edgejs/napi/quickjs/src/internal/napi_shared_array_buffer.h
~/src/edgejs/napi/quickjs/src/internal/napi_shared_array_buffer.cc
~/src/edgejs/napi/quickjs/src/unofficial_napi.cc
~/src/edgejs/napi/quickjs/deps/quickjs/quickjs.h
```

Proposed callgraph:

```text
JavaScript port.postMessage(sharedArrayBuffer)
  -> Node worker/message_port layer
  -> QuickJS N-API serializer
  -> JS_WriteObject2(..., WRITE_OBJ_SAB, JSSABTab)
  -> serialized payload records SAB identity/table entry
  -> JS_ReadObject2(..., JSSABTab)
  -> receiver gets a real SharedArrayBuffer backed by shared storage
```

The allocator/refcount helper belongs in N-API's QuickJS integration because it
bridges QuickJS SAB lifetime with Node/N-API ownership.

## Proposed Solution References

### [wasmerio/edgejs#91: [WIP] Node tests using Edgejs WASIX QuickJS](https://github.com/wasmerio/edgejs/pull/91)

- Sadhbh: edgejs [38e01f79](https://github.com/wasmerio/edgejs/commit/38e01f79c15636badbab17faf6c31eef7d16abc6) Napi/Qjs: SharedArrayBuffer. Ssl certs in wasix tests.

### Commits without PR

- Sadhbh: napi [21f22e3](https://github.com/wasmerio/napi/commit/21f22e3cc3b90a2c453d2e4115e0db42c5d8f68a) QJS: Serdes for SharedArrayBuffer
