# Mount Resolver and Certificate Files Instead of Hard-Coding Answers

Why this is a problem:

The guest needs normal files such as `/etc/hosts` and SSL certs. Hard-coding
loopback answers in EdgeJS or synthesizing files in Wasmer hides a missing
package/runtime configuration problem.

When it occurs:

- DNS loopback and `localhost` tests;
- TLS tests needing CA/cert fixtures;
- package execution in GitHub where host paths differ from local paths.

Minimal manifestation:

```js
require('node:dns').lookup('localhost', console.log);
require('node:https').get('https://localhost:8443/', console.log);
```

Proposed solution:

Add real package files and mount them with `wasmer.toml` or the test runner:

```toml
[[command]]
name = "edge"
module = "edgejs"

[[command.volumes]]
source = "./etc"
target = "/etc"
```

Runner sketch:

```sh
wasmer run --net \
  --volume "$EDGEJS_ROOT/quickjs-wasm/etc:/etc" \
  --volume "$EDGEJS_ROOT/ssl-certs:/usr/local/ssl" \
  "$WASIX_EDGEJS_PACKAGE_DIR" -- "$test"
```

## Proposed Solution References

Proposed solution can be found in:

**Reference commits:**

```text
edgejs dc9a0465 Added /etc/hosts, removed stream type normalization
edgejs 38e01f79 Napi/Qjs: SharedArrayBuffer. Ssl certs in wasix tests.
```
