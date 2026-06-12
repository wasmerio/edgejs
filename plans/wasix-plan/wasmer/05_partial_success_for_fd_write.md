# Partial Success for `fd_write`

Why this is a problem:

POSIX-like write APIs can report partial success. If the first iovec writes and
a later iovec fails, returning total failure loses bytes already accepted by the
kernel/runtime and can confuse stream accounting.

When it occurs:

- process stdout/stderr pipes;
- stream and HTTP output accounting;
- tests around bytes written and close/error ordering.

Minimal manifestation:

```c
struct iovec iov[2] = {
  { .iov_base = "ok", .iov_len = 2 },
  { .iov_base = bad_ptr, .iov_len = 4 },
};

writev(fd, iov, 2); // should be allowed to return 2
```

Proposed solution:

If one or more iovecs have already written successfully, return the successful
byte count instead of converting the later error into total failure.

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

Proposed solution can be found in:

**Reference commits:**

```text
wasmer 945aac9b18f fd_write should not fail it there was at least one success
```
