# Future: `sendmsg` / `recvmsg` Control Data and FD Passing

Why this is a problem:

Plain IPC messages can travel as stream bytes. Passing a socket or server handle
between processes requires ancillary data, normally `sendmsg()` with
`SCM_RIGHTS`. Node cluster shared UDP handles need this.

When it occurs:

- `test-dgram-cluster-*`;
- `test-dgram-bind-shared-ports`;
- cluster rows that need a worker to receive a shared handle.

Minimal manifestation:

```js
// Parent
worker.send({ cmd: 'use this socket' }, udpSocket);

// Child
process.on('message', (msg, handle) => {
  handle.on('message', () => {});
});
```

Boundary:

```text
Node cluster/fork
  -> child_process IPC channel on fd 3
  -> libuv uv_write2()
  -> sendmsg(..., SCM_RIGHTS)
  -> wasix-libc sendmsg()
  -> Wasmer sock_send_msg(control)
  -> receiver recvmsg(control)
```

Proposed solution:

Implement runtime-owned handle passing:

- parse WASIX control-message buffers;
- validate `SOL_SOCKET` + `SCM_RIGHTS`;
- duplicate sender fd entries with rights/cloexec metadata;
- queue control metadata with socketpair stream data;
- serialize received handles into the receiver fd table during `sock_recv_msg`;
- report truncation if the receiver's control buffer is too small.

Do not silently drop control data. Until implemented, returning `ENOTSUP` is
more honest than pretending descriptor passing worked.

## Proposed Solution References

Proposed solution can be found in:

**Reference status:**

```text
WITX/libc payload ABI exists as plan/input.
Phase 2 SCM_RIGHTS/handle passing remains a Wasmer runtime task.
```
