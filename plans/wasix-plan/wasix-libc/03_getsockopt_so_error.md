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

```c
int err = 0;
socklen_t len = sizeof(err);
getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len);
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

Proposed solution can be found in:

**Reference commits:**

```text
wasix-libc 13626ca Last socket error
```

**Cross-project pair:**

```text
wasmer 0780b11bb2a Last socket error
```
