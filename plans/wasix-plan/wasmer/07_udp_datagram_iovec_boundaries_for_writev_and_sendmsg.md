# UDP Datagram Iovec Boundaries for `writev()` and `sendmsg()`

Why this is a problem:

UDP preserves datagram boundaries. A single vectored send operation represents
one datagram whose payload is the concatenation of all iovec bytes. If the
runtime treats a datagram socket like a stream socket and sends each iovec
separately, packet boundaries change.

For example, one call that logically sends `"he" + "llo"` should produce one
`"hello"` datagram. If Wasmer sends each iovec independently, the receiver sees
one `"he"` datagram and one `"llo"` datagram.

We care about `writev()` now because it is implemented and reaches WASI
`fd_write`. A POSIX program can call `writev()` on a datagram socket today, so
Wasmer needs correct datagram-boundary behavior on that path. `sendmsg()` has the
same datagram-boundary rule, but full `sendmsg()` / `recvmsg()` support is a
future control-data and fd-passing task; see
[Future: `sendmsg` / `recvmsg` Control Data and FD Passing](06_future_sendmsg_recvmsg_control_data_and_fd_passing.md).

When it occurs:

- POSIX `writev()` on UDP sockets;
- future POSIX `sendmsg()` on UDP sockets;
- higher-level runtimes that batch datagram payload segments as iovecs.

Minimal Example:

```c
#include <arpa/inet.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>

int main(void) {
  int receiver = socket(AF_INET, SOCK_DGRAM, 0);
  int sender = socket(AF_INET, SOCK_DGRAM, 0);

  struct sockaddr_in addr = {0};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = 0;
  bind(receiver, (struct sockaddr *)&addr, sizeof(addr));

  socklen_t len = sizeof(addr);
  getsockname(receiver, (struct sockaddr *)&addr, &len);
  connect(sender, (struct sockaddr *)&addr, sizeof(addr));

  struct iovec iov[2] = {
    {.iov_base = "he", .iov_len = 2},
    {.iov_base = "llo", .iov_len = 3},
  };
  writev(sender, iov, 2);

  char buf[16] = {0};
  ssize_t n = recvfrom(receiver, buf, sizeof(buf), 0, NULL, NULL);
  printf("received %zd bytes: %.*s\n", n, (int)n, buf);

  close(sender);
  close(receiver);
}
```

Callgraph and boundary:

Current problematic `writev()` path:

```text
C writev(sender, ["he", "llo"])
  -> wasix-libc writev()
  -> WASI fd_write
  -> Wasmer lib/wasix/src/syscalls/wasi/fd_write.rs fd_write_internal(...)
     -> socket fd branch iterates iovecs
     -> InodeSocket::send("he")
     -> InodeSocket::send("llo")
        HERE IS THE PROBLEM: two UDP datagrams are emitted instead of one
```

Future `sendmsg()` path has the same datagram-boundary requirement:

```text
C sendmsg(sender, msg_iov=["he", "llo"], control=...)
  -> wasix-libc sendmsg()
  -> WASIX sendmsg-capable syscall boundary
  -> Wasmer sendmsg implementation
     -> payload iovecs must be one datagram
     -> control data / SCM_RIGHTS handling is separate future work
```

Proposed solution:

- Detect datagram sockets on vectored send paths.
- For datagram sockets, copy all iovec bytes into one temporary packet and send
  exactly one datagram.
- Keep stream sockets on the existing per-iovec/partial-progress behavior.
- Keep `sendmsg()` control-data and fd-passing work in the future `sendmsg()` /
  `recvmsg()` plan; this page is only about payload datagram boundaries.

Relevant Wasmer code paths:

```text
lib/wasix/src/syscalls/wasi/fd_write.rs
  fd_write_internal(...)
    socket fd branch for POSIX writev()

lib/wasix/src/syscalls/wasix/sock_send.rs
  sock_send_internal(...)
    existing WASIX send path with datagram iovec coalescing behavior

lib/wasix/src/syscalls/wasix/sock_send_to.rs
  sock_send_to_internal(...)
    send-to datagram path should preserve one syscall as one datagram
```

Proposed callgraph:

```text
C writev(sender, ["he", "llo"])
  -> wasix-libc writev()
  -> WASI fd_write
  -> Wasmer fd_write_internal(...)
     -> detect datagram socket
     -> coalesce iovecs into "hello"
     -> InodeSocket::send("hello") once
     -> receiver gets exactly one datagram
```

Sketch:

```rust
fn datagram_iov_send(iovs: &[IoSlice<'_>], peer: SocketAddr) -> Result<usize, Errno> {
    let len: usize = iovs.iter().map(|iov| iov.len()).sum();
    let mut packet = Vec::with_capacity(len);
    for iov in iovs {
        packet.extend_from_slice(iov);
    }

    socket.send_to(&packet, peer).map_err(map_socket_err)?;
    Ok(len)
}
```

## Proposed Solution References

### [wasmerio/wasmer#6685: Udp datagram receive and last err](https://github.com/wasmerio/wasmer/pull/6685)

- Sadhbh: wasmer [50e5c1f1375](https://github.com/Anodized-Titanium/wasmer/commit/50e5c1f137523e683e1b52a8f847dcdc35ca0b37) Connected UDP send iovec coalescing.

### Related Plan

- [Future: `sendmsg` / `recvmsg` Control Data and FD Passing](06_future_sendmsg_recvmsg_control_data_and_fd_passing.md)
