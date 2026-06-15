# `getsockopt(SO_ERROR)`

Why this is a problem:

In the baseline libc wrapper, `getsockopt(SOL_SOCKET, SO_ERROR)` does not expose
Wasmer's socket last-error state through normal POSIX `getsockopt()` semantics.
That means a C caller can complete a nonblocking connect, ask for `SO_ERROR`,
and fail to receive the actual pending socket error.

The fix will translate `SO_ERROR` into the WASIX last-error socket option,
return the result as an `int`, and update `*optlen` the way POSIX callers
expect.

Minimal Example:

This is a complete C program that uses a nonblocking connect and then asks libc
for the pending socket error through the normal POSIX `SO_ERROR` path. Without
the fix, libc can report success or a generic value instead of the runtime's
last socket error.

```c
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

int main(void) {
  int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
  if (fd < 0) {
    perror("socket");
    return 1;
  }

  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port = htons(1);
  if (inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr) != 1) {
    perror("inet_pton");
    return 1;
  }

  int rc = connect(fd, (struct sockaddr *)&addr, sizeof(addr));
  if (rc < 0 && errno != EINPROGRESS) {
    perror("connect");
    return 1;
  }

  struct pollfd pfd = { .fd = fd, .events = POLLOUT };
  poll(&pfd, 1, 1000);

  int err = 0;
  socklen_t len = sizeof(err);
  if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) < 0) {
    perror("getsockopt(SO_ERROR)");
    return 1;
  }

  printf("SO_ERROR=%d (%s), len=%u\n", err, strerror(err), (unsigned)len);
  close(fd);
  return err == 0 ? 1 : 0;
}
```

Callgraph and boundary:

Current problematic path:

```text
C getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len)
  -> wasix-libc libc-top-half/musl/src/network/getsockopt.c
  -> wasix-libc libc-bottom-half/cloudlibc/src/libc/sys/socket/getsockopt.c
     -> no correct SO_ERROR mapping to WASIX LastError
        HERE IS THE PROBLEM: libc cannot ask runtime for pending socket error
  -> caller receives success/unsupported/wrong value
     HERE IS THE PROBLEM: POSIX SO_ERROR contract is not met
```

Proposed solution:

Translate `SOL_SOCKET/SO_ERROR` into the WASIX last-error socket option, return
an `int`, and keep normal `getsockopt()` pointer/length semantics.

Relevant wasix-libc code paths:

```text
libc-top-half/musl/src/network/getsockopt.c
  POSIX-facing wrapper

libc-bottom-half/cloudlibc/src/libc/sys/socket/getsockopt.c
  detect SOL_SOCKET + SO_ERROR
  call __wasi_sock_get_opt_size(fd, Sockoption::LastError, &value)
  copy int value into optval and set *optlen

libc-bottom-half/headers/public/wasi/api_wasix.h
  __wasi_sock_get_opt_size(...)
```

Proposed callgraph:

```text
C getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len)
  -> wasix-libc getsockopt()
  -> recognize SO_ERROR
  -> __wasi_sock_get_opt_size(fd, LastError, &last_error)
  -> err = (int)last_error
  -> len = sizeof(int)
  -> caller receives pending socket error through POSIX API
```

## Proposed Solution References

### [wasix-org/wasix-libc#116: Fix socket options and last error](https://github.com/wasix-org/wasix-libc/pull/116)

- Sadhbh: wasix-libc [13626ca](https://github.com/Anodized-Titanium/wasix-libc/commit/13626cacc589cd284f97bd724715fb0184e2bc38) Last socket error

### [wasmerio/wasmer#6685: Udp datagram receive and last err](https://github.com/wasmerio/wasmer/pull/6685)

- Sadhbh: wasmer [0780b11bb2a](https://github.com/Anodized-Titanium/wasmer/commit/0780b11bb2abc4bfd532695c5887b215f6efbed7) Last socket error
