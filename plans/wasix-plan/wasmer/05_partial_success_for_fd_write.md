# Partial Success for `fd_write`

Why this is a problem:

In the baseline runtime, a multi-iovec write can be treated as all-or-nothing.
If the first iovec writes successfully and a later iovec fails, Wasmer can return
the later error for the whole call. That hides bytes already accepted by the fd
and breaks POSIX-style partial write accounting.

The fix will make `fd_write` preserve partial success. If at least one byte has
already been written, a later iovec error should not erase that success. The
syscall should report the successful byte count to the guest.

When it occurs:

- process stdout/stderr pipes;
- stream and HTTP output accounting;
- tests around bytes written and close/error ordering.

Minimal Example:

```c
#include <sys/uio.h>
#include <unistd.h>

int main(void) {
  char ok[] = "ok";
  char *bad_ptr = (char *)0x1;
  struct iovec iov[2] = {
    { .iov_base = ok, .iov_len = 2 },
    { .iov_base = bad_ptr, .iov_len = 4 },
  };

  return writev(1, iov, 2) < 0;
}
```

Callgraph and boundary:

Current problematic path:

```text
C writev(fd, [valid_iov, invalid_or_failing_iov])
  -> wasix-libc writev()
  -> __wasi_fd_write(fd, iovs, iovs_len, &nwritten)
  -> Wasmer lib/wasix/src/syscalls/wasi/fd_write.rs fd_write_internal(...)
     -> writes first iov successfully
     -> later iov fails
        HERE IS THE PROBLEM: function can return Err(...) for the whole call
     -> guest sees failure and loses the already-written byte count
        HERE IS THE PROBLEM: POSIX partial success is hidden
```

Proposed solution:

If one or more iovecs have already written successfully, return the successful
byte count instead of converting the later error into total failure.

Relevant Wasmer code paths:

```text
lib/wasix/src/syscalls/wasi/fd_write.rs
  fd_write(...)
  fd_write_internal(...)
    track bytes_written across iovs
    if an error happens after bytes_written > 0, return Ok(bytes_written)

lib/wasix/src/syscalls/wasix/sock_send.rs
  routes socket send fallback through fd_write_internal for pipe-like fds

lib/wasix/src/journal/effector/syscalls/fd_write.rs
  replay path should preserve the same partial-write semantics
```

Proposed callgraph:

```text
C writev(fd, [valid_iov, invalid_or_failing_iov])
  -> Wasmer fd_write_internal(...)
  -> write valid_iov, bytes_written += n
  -> later iov returns error
  -> because bytes_written > 0, return Ok(bytes_written)
  -> guest observes partial successful write
```

Sketch:

```rust
let mut written = 0;
for iov in iovs {
    match write_one(iov) {
        Ok(n) => written += n,
        Err(err) if written > 0 => return Ok(written),
        Err(err) => return Err(err),
    }
}
Ok(written)
```

## Proposed Solution References

### [wasmerio/wasmer#6685: Udp datagram receive and last err](https://github.com/wasmerio/wasmer/pull/6685)

- Sadhbh: wasmer [945aac9b18f](https://github.com/Anodized-Titanium/wasmer/commit/945aac9b18fafbbee9b5605e6f57211dd94afaf8) fd_write should not fail it there was at least one success
