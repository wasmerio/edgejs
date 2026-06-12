# Socket Type Flags: `SOCK_NONBLOCK` and `SOCK_CLOEXEC`

Why this is a problem:

POSIX allows callers to pass `SOCK_NONBLOCK` and `SOCK_CLOEXEC` in the socket
type. libuv does this on Unix:

```c
socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
```

If libc forwards those bits as the socket type, the runtime may not recognize
the type as `SOCK_DGRAM` or `SOCK_STREAM`. If libc ignores the flags, the fd can
be blocking and libuv's drain loops can hang.

When it occurs:

- UDP tests that should get `EAGAIN`;
- HTTP/TCP sockets created through libuv;
- any socket created via the standard Unix fast path.

Boundary:

```text
libuv uv__socket()
  -> socket(domain, type | SOCK_NONBLOCK | SOCK_CLOEXEC, protocol)
  -> wasix-libc socket()
  -> __wasi_sock_open(domain, base_type, protocol, &fd)
  -> fcntl(fd, F_SETFL, O_NONBLOCK)
```

Proposed solution:

Mask the base socket type before calling WASIX, then apply flags through the
normal fd flag paths.

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
