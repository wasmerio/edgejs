# Last Socket Error / `SO_ERROR`

Why this is a problem:

Node and libuv check `getsockopt(SO_ERROR)` after nonblocking connect and write
paths. If Wasmer loses the actual socket error, JavaScript sees the wrong error,
for example `EPIPE` or `ECONNRESET` where the test expects `ECONNREFUSED`.

When it occurs:

- refused HTTP/TLS connects;
- proxy connection-refused tests;
- tests asserting exact Node error codes.

Minimal manifestation:

```js
const net = require('node:net');

net.connect({ host: '127.0.0.1', port: 9 })
  .on('error', (err) => console.log(err.code));
```

Boundary:

```text
libuv uv__stream_connect()
  -> getsockopt(fd, SOL_SOCKET, SO_ERROR, ...)
  -> wasix-libc __wasi_sock_get_opt_*()
  -> Wasmer socket last_error
```

Proposed solution:

Record the last connect/send/recv error on the Wasmer socket object and expose
it through the WASIX socket-option path. Reading `SO_ERROR` should return and,
where POSIX requires it, clear the pending error.

Sketch:

```rust
struct WasiSocket {
    last_error: Option<Errno>,
}

fn set_last_socket_error(&mut self, err: Errno) {
    self.last_error = Some(err);
}

fn get_so_error(&mut self) -> Errno {
    self.last_error.take().unwrap_or(Errno::Success)
}
```

## Proposed Solution References

Proposed solution can be found in:

**Reference commits:**

```text
wasmer 0780b11bb2a Last socket error
```

**Cross-project pair:**

```text
wasix-libc 13626ca Last socket error
```
