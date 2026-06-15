# EdgeJS: OpenSSL error mapping and version capability

Why this is a problem:

Node crypto and TLS tests assert exact error classes, OpenSSL reasons, and
feature availability. If EdgeJS uses an older OpenSSL build, version-gated
algorithms are unavailable. If the binding maps the wrong OpenSSL error from the
error stack, JavaScript receives a misleading Node error even when the lower
socket/TLS behavior is correct.

This occurs in crypto key generation, PQC/WebCrypto, TLS parser, and TLS
handshake tests that inspect the exact error code or expected algorithm support.

Why native EdgeJS passes, and why WASIX still needs this:

Native EdgeJS was built and tested against the native OpenSSL dependency and the
error-stack behavior used by that build. The WASIX build can lag behind in two
ways: it may compile a different OpenSSL version, and the WASIX/QuickJS path can
exercise slightly different failure ordering around TLS/socket errors. If EdgeJS
selects the wrong OpenSSL stack entry, native may still pass because the stack is
shaped differently there, while WASIX exposes the wrong Node-visible code. The
valid EdgeJS work is to make the OpenSSL dependency and error mapping match the
Node contract for all builds; lower layers still own socket `SO_ERROR`, but
EdgeJS owns translating OpenSSL reasons into JavaScript errors.

Minimal Example:

```js
const assert = require('node:assert');
const crypto = require('node:crypto');
const tls = require('node:tls');

assert.doesNotThrow(() => crypto.getHashes());

const socket = tls.connect({ port: 9, host: '127.0.0.1', rejectUnauthorized: false });
socket.on('error', (err) => {
  // Tests care that this is the right Node/OpenSSL-facing error, not a stale or
  // unrelated OpenSSL stack entry.
  assert(err.code || err.reason || err.message);
});
```

Failing tests that showed this class:

```text
parallel/test-crypto-keygen-*
parallel/test-crypto-pqc-key-objects-*
parallel/test-webcrypto-derivebits-argon2
parallel/test-webcrypto-sign-verify-eddsa
parallel/test-tls-hello-parser-failure
sequential/test-tls-connect
```

Callgraph and boundary:

Current problematic path:

```text
JavaScript crypto/tls operation
  -> EdgeJS src/internal_binding/binding_crypto.cc
  -> OpenSSL API fails or reports a reason
  -> EdgeJS MapOpenSslErrorCode()/CreateOpenSslError()
     HERE IS THE PROBLEM: the binding can select the wrong error stack entry or
     lack mappings for newer OpenSSL reasons, so JS sees the wrong code.

JavaScript crypto operation requiring newer OpenSSL capability
  -> EdgeJS OpenSSL dependency
     HERE IS THE PROBLEM: older WASIX OpenSSL builds do not expose algorithms
     that Node tests gate on OpenSSL 3.5 behavior.
```

The boundary is EdgeJS <-> OpenSSL. Wasmer should preserve socket errors and
libc should expose `SO_ERROR`, but OpenSSL-to-Node error translation is owned by
EdgeJS.

Proposed solution:

Update the WASIX OpenSSL dependency/build to the intended OpenSSL version and
make EdgeJS error mapping choose the relevant high-level OpenSSL reason for the
Node error being constructed.

Relevant EdgeJS code paths:

```text
~/src/edgejs/deps/openssl-wasix
~/src/edgejs/src/internal_binding/binding_crypto.cc
~/src/edgejs/src/crypto
~/src/edgejs/test/parallel/test-crypto-*.js
~/src/edgejs/test/parallel/test-tls-*.js
```

Proposed callgraph:

```text
JavaScript crypto/tls operation
  -> EdgeJS calls OpenSSL 3.5-capable dependency
  -> OpenSSL reports failure/reason stack
  -> EdgeJS selects the relevant reason for this operation
  -> EdgeJS maps it to the Node-compatible code/reason/message
  -> JavaScript observes the expected error class
```

This is intentionally not a generic Wasmer workaround: the runtime cannot know
which OpenSSL stack entry should become a Node.js error.

## Proposed Solution References

### [wasmerio/edgejs#91: [WIP] Node tests using Edgejs WASIX QuickJS](https://github.com/wasmerio/edgejs/pull/91)

- Sadhbh: edgejs [5254f803](https://github.com/wasmerio/edgejs/commit/5254f803ff93200d79231d87501f813eb1b35147) OpenSSL update to 3.5
- Sadhbh: edgejs [b9ef182f](https://github.com/wasmerio/edgejs/commit/b9ef182f2322eb0e111f40427e4685852ec6a241) Fixes for OpenSSL error code mapping
