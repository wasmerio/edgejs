# Boolean Socket Option Pointer Handling

Why this is a problem:

In the baseline libc socket option wrapper, `setsockopt()` can treat `optval` as
the option value instead of reading the integer stored at `optval`. That makes
boolean options depend on the pointer address. Since a valid stack pointer is
almost always nonzero, a caller trying to set an option to `0` can accidentally
set it to `1`.

The fix will make libc validate `optlen`, copy the pointed-to integer, and pass
that integer's boolean meaning to the WASIX socket option syscall.

When it occurs:

- `IPV6_V6ONLY = 0`;
- keepalive toggles;
- local-address/autoselect-family tests.

Minimal Example:

This is a complete C program for the specific bug shape: the caller passes a
pointer to an integer value of `0`. Libc must read `*optval`; it must not treat
the pointer address itself as the boolean value.

```c
#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

int main(void) {
  int fd = socket(AF_INET6, SOCK_STREAM, 0);
  if (fd < 0) {
    perror("socket");
    return 1;
  }

  int off = 0;
  if (setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &off, sizeof(off)) < 0) {
    perror("setsockopt(IPV6_V6ONLY=0)");
    return 1;
  }

  int value = -1;
  socklen_t value_len = sizeof(value);
  if (getsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &value, &value_len) < 0) {
    perror("getsockopt(IPV6_V6ONLY)");
    return 1;
  }

  if (value != 0) {
    fprintf(stderr, "expected IPV6_V6ONLY=0, got %d\n", value);
    return 1;
  }

  puts("boolean socket option used pointed-to value");
  close(fd);
  return 0;
}
```

Callgraph and boundary:

Current problematic path:

```text
C setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &off, sizeof(off))
  -> wasix-libc libc-top-half/musl/src/network/setsockopt.c
  -> wasix-libc libc-bottom-half/cloudlibc/src/libc/sys/socket/setsockopt.c
     -> receives option_value = &off
     -> may convert option_value pointer itself to boolean
        HERE IS THE PROBLEM: stack address is nonzero even when `off == 0`
  -> __wasi_sock_set_opt_flag(fd, IPV6_V6ONLY, true)
     HERE IS THE PROBLEM: caller asked to disable the option, runtime enables it
```

Proposed solution:

Read the pointed-to integer/boolean value after validating `optlen`, then pass
that value through the WASIX socket option syscall.

Relevant wasix-libc code paths:

```text
libc-top-half/musl/src/network/setsockopt.c
  POSIX-facing wrapper

libc-bottom-half/cloudlibc/src/libc/sys/socket/setsockopt.c
  validate option_len
  memcpy integer value from option_value
  lower boolean options to __wasi_sock_set_opt_flag(...)
  lower size/time options to matching WASIX calls
```

Proposed callgraph:

```text
C setsockopt(..., &off, sizeof(off)) where off == 0
  -> wasix-libc setsockopt()
  -> validate option_len >= sizeof(int)
  -> memcpy enabled from option_value
  -> enabled = 0
  -> __wasi_sock_set_opt_flag(fd, IPV6_V6ONLY, false)
  -> runtime receives the caller's intended value
```

Sketch:

```c
if (optlen < sizeof(int)) {
  errno = EINVAL;
  return -1;
}

int enabled;
memcpy(&enabled, optval, sizeof(enabled));
return __wasi_sock_set_opt_flag(fd, level, optname, enabled != 0);
```

## Proposed Solution References

### [wasix-org/wasix-libc#116: Fix socket options and last error](https://github.com/wasix-org/wasix-libc/pull/116)

- Sadhbh: wasix-libc [17e686c](https://github.com/Anodized-Titanium/wasix-libc/commit/17e686c6a1cd6f11b3085aa23a42c0263fe99de5) Socket option incorrectly deferenced pointer
