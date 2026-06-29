# Remaining Unsatisfied Networking Plan

These are not part of the local-pass / GitHub-fail recovery set, but they are
the next runtime/libc networking plan items:

- UNIX domain sockets / named pipes for HTTP/TLS pipe tests.
- `SCM_RIGHTS` descriptor passing for cluster shared handles.
- UDP multicast semantics for local and external network cases.
- UDP socket options such as receive buffer size and TTL.
- TCP keepalive options in Wasmer/`wasix-libc` rather than long-term libuv
  no-ops.

The intended ownership remains:

```text
WITX / wasix-libc ABI if POSIX needs a new exposed syscall shape
Wasmer if the OS/runtime behavior is missing
libuv-wasix only for a temporary POSIX-facing adaptation
EdgeJS only when the behavior is genuinely guest-owned
```
