# Last Socket Error / `SO_ERROR`

Why this is a problem:

In the baseline runtime, the socket operation that actually fails and the later
`getsockopt(SO_ERROR)` call can be disconnected from each other. A nonblocking
connect can fail with `ECONNREFUSED`, but if Wasmer does not record that error
on the socket object, the next `SO_ERROR` read can return success or a generic
error.

The visible result is wrong POSIX error reporting. Code that expects
`ECONNREFUSED` may instead see `EPIPE`, `ECONNRESET`, or no pending error at
all. The fix will store the last socket error in Wasmer socket state and expose
that state through the WASIX `Sockoption::LastError` path used by
`getsockopt(SO_ERROR)`.

When it occurs:

- refused HTTP/TLS connects;
- proxy connection-refused tests;
- tests asserting exact connect or write error codes.

Minimal Example:

```c
#include <arpa/inet.h>
#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

int main(void) {
  int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);

  struct sockaddr_in addr = {0};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = htons(9); /* normally closed discard port */

  connect(fd, (struct sockaddr *)&addr, sizeof(addr));

  struct pollfd pfd = {.fd = fd, .events = POLLOUT};
  poll(&pfd, 1, 1000);

  int err = 0;
  socklen_t errlen = sizeof(err);
  getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &errlen);
  printf("SO_ERROR: %s\n", strerror(err));

  close(fd);
}
```

Callgraph and boundary:

Current problematic path:

```text
C connect(fd, closed_loopback_port)
  -> wasix-libc connect()
  -> Wasmer lib/wasix/src/net/socket.rs InodeSocket::connect(...)
  -> virtual-net TCP connect
     -> host OS reports ECONNREFUSED
     -> Wasmer maps the immediate operation error
        HERE IS THE PROBLEM: error may not be retained as socket last_error

C poll(fd, POLLOUT)
  -> readiness completes because connection attempt is finished

C getsockopt(fd, SOL_SOCKET, SO_ERROR)
  -> wasix-libc getsockopt()
  -> __wasi_sock_get_opt_size(Sockoption::LastError)
  -> Wasmer lib/wasix/src/syscalls/wasix/sock_get_opt_size.rs
  -> socket.last_error()
     -> no preserved error, success, or wrong generic error
        HERE IS THE PROBLEM: caller cannot recover original ECONNREFUSED
```

Proposed solution:

Record the last connect/send/recv error on the Wasmer socket object and expose
it through the WASIX socket-option path. Reading `SO_ERROR` should return and,
where POSIX requires it, clear the pending error.

Relevant Wasmer code paths:

```text
lib/wasix/src/net/socket.rs
  InodeSocket::connect(...)
  InodeSocket::send(...)
  InodeSocket::recv_from(...)
    record meaningful socket errors into socket state

  InodeSocket::last_error(...)
    return the stored socket error as Errno

lib/wasix/src/syscalls/wasix/sock_get_opt_size.rs
  Sockoption::LastError
    return socket.last_error()

lib/virtual-net/src/host.rs
  TcpStream/UDP socket error mapping

lib/virtual-net/src/lib.rs
  VirtualSocket::last_error(...)
```

Proposed callgraph:

```text
C connect(fd, closed_loopback_port)
  -> Wasmer InodeSocket::connect(...)
  -> virtual-net returns NetworkError::ConnectionRefused
  -> Wasmer maps to Errno::Connrefused and records socket.last_error

C getsockopt(fd, SOL_SOCKET, SO_ERROR)
  -> wasix-libc translates SO_ERROR to WASIX LastError
  -> Wasmer sock_get_opt_size(Sockoption::LastError)
  -> InodeSocket::last_error()
  -> returns ECONNREFUSED to C caller
```

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

### [wasix-org/wasix-libc#116: Fix socket options and last error](https://github.com/wasix-org/wasix-libc/pull/116)

- Sadhbh: wasix-libc [13626ca](https://github.com/Anodized-Titanium/wasix-libc/commit/13626cacc589cd284f97bd724715fb0184e2bc38) Last socket error

### [wasmerio/wasmer#6685: Udp datagram receive and last err](https://github.com/wasmerio/wasmer/pull/6685)

- Sadhbh: wasmer [0780b11bb2a](https://github.com/Anodized-Titanium/wasmer/commit/0780b11bb2abc4bfd532695c5887b215f6efbed7) Last socket error
