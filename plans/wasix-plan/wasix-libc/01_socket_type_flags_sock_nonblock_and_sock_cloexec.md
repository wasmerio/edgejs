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

This is a complete C program that asks libc for a UDP socket that is both
nonblocking and close-on-exec at creation time. Without the fix, libc can pass
the combined type bits to WASIX as if they were the base socket type, or it can
create the socket but lose one of the requested flags.

```c
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

int main(void) {
  int fd = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
  if (fd < 0) {
    perror("socket");
    return 1;
  }

  int status_flags = fcntl(fd, F_GETFL, 0);
  if (status_flags < 0) {
    perror("fcntl(F_GETFL)");
    return 1;
  }
  if ((status_flags & O_NONBLOCK) == 0) {
    fprintf(stderr, "socket is not nonblocking\n");
    return 1;
  }

  int fd_flags = fcntl(fd, F_GETFD, 0);
  if (fd_flags < 0) {
    perror("fcntl(F_GETFD)");
    return 1;
  }
  if ((fd_flags & FD_CLOEXEC) == 0) {
    fprintf(stderr, "socket is not close-on-exec\n");
    return 1;
  }

  char byte;
  ssize_t nread = recv(fd, &byte, sizeof(byte), 0);
  if (nread < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
    puts("socket flags are visible through POSIX APIs");
    close(fd);
    return 0;
  }

  fprintf(stderr, "expected nonblocking recv, got nread=%zd errno=%d (%s)\n",
          nread, errno, strerror(errno));
  return 1;
}
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

### [wasix-org/wasix-libc#116: Fix socket options and last error](https://github.com/wasix-org/wasix-libc/pull/116)

- Sadhbh: wasix-libc [c0853db](https://github.com/Anodized-Titanium/wasix-libc/commit/c0853db552fc0b85a23cdbe35a4f1d5b37c3cb8a) Open sockets with nonblock|cloexec flags correctly
