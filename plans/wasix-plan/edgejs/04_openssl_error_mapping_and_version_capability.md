# OpenSSL Error Mapping and Version Capability

Why this is a problem:

Node tests often assert specific OpenSSL-backed error classes and messages. If
the binding maps the wrong item from the OpenSSL error stack, JavaScript gets a
different code from native Node. Some crypto/PQC tests also require OpenSSL 3.5
capability.

When it occurs:

- TLS parser/error tests;
- crypto key generation tests;
- PQC algorithm tests.

Minimal manifestation:

```js
const tls = require('node:tls');
const s = tls.connect({ port: 1 });
s.on('error', (err) => console.log(err.code, err.message));
```

Boundary:

```text
OpenSSL C API
  -> EdgeJS crypto/tls binding
  -> Node Error object
  -> JS test assertion
```

Proposed solution:

Use OpenSSL 3.5 for the WASIX OpenSSL dependency and map OpenSSL error stack
entries into Node-visible errors using the same reason/code selection native
Node expects.

Sketch:

```cpp
unsigned long err = ERR_peek_last_error();
int reason = ERR_GET_REASON(err);
return MakeNodeCryptoError(env, reason);
```

## Proposed Solution References

Proposed solution can be found in:

**Reference commits:**

```text
edgejs 5254f803 OpenSSL update to 3.5
edgejs b9ef182f Fixes for OpenSSL error code mapping
```
