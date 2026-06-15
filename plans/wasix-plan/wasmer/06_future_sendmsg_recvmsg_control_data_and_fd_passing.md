# Future: `sendmsg` / `recvmsg` Control Data and FD Passing

Why this is a problem:

In the baseline runtime, payload bytes on a socketpair can move, but ancillary
control data is not a real runtime object. POSIX `SCM_RIGHTS` is not just bytes
attached to a message. It means the sending process asks the OS to duplicate or
transfer fd table entries into the receiving process with the correct rights,
lifetime, and close-on-exec behavior.

If Wasmer accepts `sendmsg()` control data but drops it, the receiver gets a
normal payload byte and no usable handle. If Wasmer returns `ENOTSUP`, the
failure is honest but cluster-style shared handle tests cannot pass. The fix
will make `sock_send_msg` and `sock_recv_msg` carry runtime-owned fd metadata
alongside stream data.

When it occurs:

- `test-dgram-cluster-*`;
- `test-dgram-bind-shared-ports`;
- cluster rows that need a worker to receive a shared handle.

Minimal Example:

```c
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

int main(void) {
  int sv[2];
  socketpair(AF_UNIX, SOCK_STREAM, 0, sv);

  int fd_to_pass = sv[0];
  char byte = 'x';
  char control[CMSG_SPACE(sizeof(int))];

  struct iovec iov = {.iov_base = &byte, .iov_len = 1};
  struct msghdr msg = {0};
  msg.msg_iov = &iov;
  msg.msg_iovlen = 1;
  msg.msg_control = control;
  msg.msg_controllen = sizeof(control);

  struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
  cmsg->cmsg_level = SOL_SOCKET;
  cmsg->cmsg_type = SCM_RIGHTS;
  cmsg->cmsg_len = CMSG_LEN(sizeof(int));
  memcpy(CMSG_DATA(cmsg), &fd_to_pass, sizeof(int));

  sendmsg(sv[0], &msg, 0);

  char out;
  char recv_control[CMSG_SPACE(sizeof(int))];
  struct iovec recv_iov = {.iov_base = &out, .iov_len = 1};
  struct msghdr recv_msg = {0};
  recv_msg.msg_iov = &recv_iov;
  recv_msg.msg_iovlen = 1;
  recv_msg.msg_control = recv_control;
  recv_msg.msg_controllen = sizeof(recv_control);

  recvmsg(sv[1], &recv_msg, 0);
  struct cmsghdr *received = CMSG_FIRSTHDR(&recv_msg);
  printf("received control type: %d\n", received ? received->cmsg_type : -1);
}
```

Callgraph and boundary:

Current problematic path:

```text
C socketpair(AF_UNIX, SOCK_STREAM, 0, sv)
  -> wasix-libc socketpair()
  -> Wasmer creates connected pipe/socketpair-like fds

C sendmsg(sv[0], payload + SCM_RIGHTS(fd_to_pass))
  -> wasix-libc sendmsg()
  -> __wasi_sock_send_msg(..., control_ptr, control_len)
  -> Wasmer sock_send_msg(...)
     -> payload path can send normal bytes
     -> control path returns ENOTSUP or ignores metadata
        HERE IS THE PROBLEM: fd table entry is not duplicated into receiver

C recvmsg(sv[1], ...)
  -> wasix-libc recvmsg()
  -> __wasi_sock_recv_msg(..., ro_control, ro_control_len)
  -> Wasmer sock_recv_msg(...)
     -> payload byte may arrive
     -> no queued rights metadata exists
        HERE IS THE PROBLEM: receiver has no fd to install into its fd table
```

Proposed solution:

Implement runtime-owned handle passing:

- parse WASIX control-message buffers;
- validate `SOL_SOCKET` + `SCM_RIGHTS`;
- duplicate sender fd entries with rights/cloexec metadata;
- queue control metadata with socketpair stream data;
- serialize received handles into the receiver fd table during `sock_recv_msg`;
- report truncation if the receiver's control buffer is too small.

Relevant Wasmer code paths:

```text
lib/wasix/src/syscalls/wasix/sock_send_msg.rs
  parse incoming control buffer
  convert SOCKET/RIGHTS control entries into runtime fd references

lib/wasix/src/syscalls/wasix/sock_recv_msg.rs
  deliver queued control entries
  allocate receiver-side fd numbers
  write cmsghdr-compatible control bytes back to guest memory

lib/wasix/src/fs/mod.rs
  fd table duplication, rights, cloexec, and target fd allocation

lib/wasix/src/net/socket.rs
  socketpair / DuplexPipe / pipe-backed IPC transport state
```

Proposed callgraph:

```text
C sendmsg(sv[0], payload + SCM_RIGHTS(fd_to_pass))
  -> wasix-libc sendmsg()
  -> Wasmer sock_send_msg(...)
  -> parse control message as fd_to_pass
  -> duplicate sender fd entry and rights into queued control metadata
  -> send payload byte plus queued control metadata through socketpair state

C recvmsg(sv[1], ...)
  -> wasix-libc recvmsg()
  -> Wasmer sock_recv_msg(...)
  -> receive payload byte
  -> allocate receiver fd for duplicated entry
  -> write SCM_RIGHTS control record containing receiver fd number
  -> C caller receives payload and a usable fd
```

Do not silently drop control data. Until implemented, returning `ENOTSUP` is
more honest than pretending descriptor passing worked.

## Proposed Solution References

### Commits without PR

- WITX/libc payload ABI exists as plan/input.
- Phase 2 SCM_RIGHTS/handle passing remains a Wasmer runtime task.
