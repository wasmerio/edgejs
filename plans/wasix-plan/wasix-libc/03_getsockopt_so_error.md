# `getsockopt(SO_ERROR)`

Why this is a problem:

libuv uses `SO_ERROR` to turn nonblocking socket completion into the correct
JavaScript error. If libc does not expose the runtime's last socket error,
EdgeJS cannot report Node-compatible errors.

Minimal manifestation:

```c
int err = 0;
socklen_t len = sizeof(err);
getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len);
```

Proposed solution:

Translate `SOL_SOCKET/SO_ERROR` into the WASIX last-error socket option, return
an `int`, and keep normal `getsockopt()` pointer/length semantics.

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
