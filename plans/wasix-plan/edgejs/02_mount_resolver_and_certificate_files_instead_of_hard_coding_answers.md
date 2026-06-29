# EdgeJS: mount resolver and certificate files instead of hard-coding answers

Why this is a problem:

A POSIX-like guest expects resolver configuration and certificate material to
come from the filesystem. If the WASIX package does not expose `/etc/hosts` or
the SSL certificate directory, EdgeJS may be tempted to hard-code `localhost`,
loopback reverse lookups, or certificate search paths. That makes EdgeJS tests
pass for the wrong reason and leaves other WASIX programs broken.

This occurs when DNS tests resolve `localhost` or TLS/HTTPS tests need the same
certificate fixtures that native Node sees.

Why native EdgeJS passes, and why WASIX still needs this:

Native EdgeJS inherits the host operating system's `/etc/hosts`, resolver
configuration, and certificate locations. The same JavaScript therefore sees a
normal `localhost` answer and OpenSSL can find the host's certificate setup or
the test fixture paths. In WASIX, the guest sees only what the package and runner
mount into its filesystem. If `/etc/hosts` or `/usr/local/ssl` is absent, EdgeJS
looks broken even though the native binary passes. The right EdgeJS-side change
is test/package configuration: mount the same kind of files a POSIX process would
have, instead of adding hard-coded resolver or TLS answers inside EdgeJS or
Wasmer.

Minimal Example:

```js
const assert = require('node:assert');
const dns = require('node:dns');
const https = require('node:https');

dns.lookup('localhost', (err, address) => {
  assert.ifError(err);
  assert(address === '127.0.0.1' || address === '::1');
});

https.get('https://localhost:443', (res) => {
  res.resume();
}).on('error', (err) => {
  // The test may expect a TLS/socket error, but not a missing cert-store or
  // missing resolver-file setup error.
  assert.notStrictEqual(err.code, 'ENOENT');
});
```

Failing tests that showed this class:

```text
parallel/test-dns
parallel/test-dns-resolveany
parallel/test-http-autoselectfamily
parallel/test-https-autoselectfamily
sequential/test-https-connect-localport
parallel/test-tls-env-extra-ca
parallel/test-tls-env-bad-extra-ca
parallel/test-tls-env-extra-ca-with-options
```

Callgraph and boundary:

Current problematic path:

```text
JavaScript dns.lookup('localhost')
  -> EdgeJS DNS binding
  -> wasix-libc resolver opens /etc/hosts
     HERE IS THE PROBLEM: package/runner may not mount /etc/hosts, so resolver
     behavior depends on hard-coded EdgeJS answers or fails differently from POSIX.

JavaScript TLS/HTTPS test
  -> EdgeJS crypto/tls binding
  -> OpenSSL default certificate lookup
     HERE IS THE PROBLEM: certificate fixture directory is not visible at the
     guest path OpenSSL expects.
```

The boundary is package/runner configuration <-> guest filesystem. Wasmer should
not synthesize project-specific files globally, and EdgeJS should not hard-code
resolver answers that belong in resolver files.

Proposed solution:

Mount the files through the EdgeJS WASIX package and runner. Keep the resolver
and TLS configuration ordinary from the guest's point of view.

Relevant EdgeJS code paths:

```text
~/src/edgejs/quickjs-wasm/wasmer.toml
~/src/edgejs/quickjs-wasm/etc/hosts
~/src/edgejs/ssl-certs
~/src/edgejs/scripts/edge-wasix-node-runner.sh
```

Proposed callgraph:

```text
wasmer run quickjs-wasm
  -> package mounts quickjs-wasm/etc at /etc
  -> package/runner mounts ssl-certs at /usr/local/ssl

JavaScript dns.lookup('localhost')
  -> wasix-libc resolver reads /etc/hosts
  -> normal POSIX-style answer is returned

JavaScript TLS/HTTPS test
  -> OpenSSL resolves cert paths inside /usr/local/ssl
  -> test observes real TLS/socket semantics instead of missing-file setup noise
```

This keeps the compatibility rule intact: configuration files are mounted like a
normal system image, not emulated inside Wasmer and not patched into EdgeJS DNS
logic.

## Proposed Solution References

### [wasmerio/edgejs#91: [WIP] Node tests using Edgejs WASIX QuickJS](https://github.com/wasmerio/edgejs/pull/91)

- Sadhbh: edgejs [dc9a0465](https://github.com/wasmerio/edgejs/commit/dc9a046509ff36b4f1e952967528480a5873e4b2) Added /etc/hosts, removed stream type normalization
- Sadhbh: edgejs [38e01f79](https://github.com/wasmerio/edgejs/commit/38e01f79c15636badbab17faf6c31eef7d16abc6) Napi/Qjs: SharedArrayBuffer. Ssl certs in wasix tests.
