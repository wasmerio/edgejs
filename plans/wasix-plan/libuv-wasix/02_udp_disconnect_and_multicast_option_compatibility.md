# UDP Disconnect and Multicast Option Compatibility

Why this is a problem:

Unix libuv uses idioms that are not currently exposed by WASIX exactly as on
Linux, such as `connect(AF_UNSPEC)` to disconnect UDP. Some multicast options
are also not implemented by the WASIX socket layer yet.

When it occurs:

- dgram disconnect tests;
- multicast TTL/interface/loopback tests;
- dgram option setters that expect support or a stable no-op.

Minimal manifestation:

```js
const dgram = require('node:dgram');
const s = dgram.createSocket('udp4');
s.setMulticastTTL(16);
s.disconnect();
```

Proposed solution:

Prefer POSIX paths where WASIX supports them. Where WASIX lacks a specific
socket operation, add the smallest libuv WASIX adaptation that preserves the
libuv contract until the runtime exposes the real operation.

Sketch:

```c
#if defined(__wasi__)
  handle->flags &= ~UV_HANDLE_UDP_CONNECTED;
  return 0;
#else
  return connect(fd, (const struct sockaddr*) &unspec, sizeof(unspec));
#endif
```

For multicast options, return success only for options that are semantically
safe to no-op in the current package target. Otherwise return a real unsupported
error so tests do not hang behind a false success.

## Proposed Solution References

Proposed solution can be found in:

**Reference commits:**

```text
libuv-wasix 0ae770b9 multicast TTL/loop/interface shims and UDP disconnect handling
```
