# Boolean Socket Option Pointer Handling

Why this is a problem:

`setsockopt()` receives a pointer to the option value. If libc accidentally
interprets the pointer address as the option value, boolean options become
nondeterministic. A pointer address is almost always nonzero, so an intended
`0` can be treated as `1`.

When it occurs:

- `IPV6_V6ONLY = 0`;
- keepalive toggles;
- local-address/autoselect-family tests.

Minimal manifestation:

```c
int off = 0;
setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &off, sizeof(off));
```

Boundary:

```text
Node/net/libuv option setup
  -> setsockopt(fd, level, name, &value, sizeof(value))
  -> wasix-libc reads pointed-to value
  -> Wasmer sock_set_opt_flag/size/time
```

Proposed solution:

Read the pointed-to integer/boolean value after validating `optlen`, then pass
that value through the WASIX socket option syscall.

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

Proposed solution can be found in:

**Reference commits:**

```text
wasix-libc 17e686c Socket option incorrectly deferenced pointer
```
