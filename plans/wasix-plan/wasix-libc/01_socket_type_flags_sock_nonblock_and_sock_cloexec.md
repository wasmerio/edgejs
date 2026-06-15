# Socket Type Flags: `SOCK_NONBLOCK` and `SOCK_CLOEXEC`

Why this is a problem:

In the baseline libc socket wrapper, the `type` argument can be forwarded to
WASIX without separating the base socket type from POSIX creation flags. That is
wrong because `SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC` is not itself a base
socket type. The runtime wants to know `SOCK_DGRAM`; libc must apply
`SOCK_NONBLOCK` and `SOCK_CLOEXEC` as fd flags.

If libc forwards the combined bits as the socket type, Wasmer may not recognize
the socket as datagram or stream. If libc opens the right socket but ignores the
flags, the fd remains blocking and code that expects `EAGAIN` can hang.

Minimal Example:

```c
#include <sys/socket.h>

int fd = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
```

When it occurs:

- UDP tests that should get `EAGAIN`;
- HTTP/TCP sockets created through Unix socket fast paths;
- any socket created with POSIX type flags.

Callgraph and boundary:

Current problematic path:

```text
C socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0)
  -> wasix-libc libc-top-half/musl/src/network/socket.c
  -> wasix-libc libc-bottom-half/cloudlibc/src/libc/sys/socket/socket.c
     -> may pass combined `ty` to __wasi_sock_open(...)
        HERE IS THE PROBLEM: runtime sees type bits, not just SOCK_DGRAM
     -> may not call fcntl(F_SETFL, O_NONBLOCK)
        HERE IS THE PROBLEM: fd can remain blocking
  -> Wasmer sock_open receives wrong type or a blocking fd
```

Proposed solution:

Mask the base socket type before calling WASIX, then apply flags through the
normal fd flag paths.

Relevant wasix-libc code paths:

```text
libc-top-half/musl/src/network/socket.c
  POSIX-facing socket() wrapper

libc-bottom-half/cloudlibc/src/libc/sys/socket/socket.c
  lower socket(domain, base_type, protocol) to __wasi_sock_open(...)
  strip SOCK_NONBLOCK and SOCK_CLOEXEC from type
  apply O_NONBLOCK / FD_CLOEXEC after fd creation

libc-bottom-half/cloudlibc/src/libc/fcntl/fcntl.c
  fd flag application path used by fcntl(...)
```

Proposed callgraph:

```text
C socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0)
  -> wasix-libc socket()
  -> base_type = SOCK_DGRAM
  -> flags = SOCK_NONBLOCK | SOCK_CLOEXEC
  -> __wasi_sock_open(AF_INET, SOCK_DGRAM, IPPROTO_UDP, &fd)
  -> fcntl(fd, F_SETFL, O_NONBLOCK)
  -> fcntl(fd, F_SETFD, FD_CLOEXEC)
  -> caller receives a datagram fd that is nonblocking and close-on-exec
```

Sketch:

```c
int socket(int domain, int type, int protocol) {
  int flags = type & (SOCK_NONBLOCK | SOCK_CLOEXEC);
  int base_type = type & ~(SOCK_NONBLOCK | SOCK_CLOEXEC);

  int fd = __wasi_sock_open(domain, base_type, protocol);

  if (flags & SOCK_NONBLOCK)
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
  if (flags & SOCK_CLOEXEC)
    fcntl(fd, F_SETFD, FD_CLOEXEC);

  return fd;
}
```

## Proposed Solution References

Proposed solution can be found in:

**Reference commits:**

```text
wasix-libc c0853db Open sockets with nonblock|cloexec flags correctly
```
