# UDP Datagram Receive, Readiness, and Backlog

Why this is a problem:

In the baseline runtime, UDP readiness can be implemented by touching the real
socket receive path. That is unsafe for UDP. A datagram is the unit of delivery,
and observing readiness must not consume or discard the packet that made the fd
readable.

The broken behavior is that `poll_oneoff()` can report `FdRead`, but the later
`recvfrom()` can see `EAGAIN`, block, or time out because the readiness path
already consumed the datagram. Zero-length UDP packets expose this quickly,
because there is no payload byte to distinguish "empty datagram delivered" from
"nothing was available" unless the runtime preserves the datagram boundary.

The fix will make readiness non-destructive. Wasmer should keep a per-socket UDP
backlog. Readiness probes may fill that backlog, but guest receive calls must be
the only path that consumes it. `peek` reads should copy from the backlog without
popping it.

When it occurs:

- zero-length UDP packets;
- recursive UDP send/receive callbacks;
- default-host dgram tests;
- any path where the guest polls first and then drains the socket.

Minimal Example:

```c
#include <arpa/inet.h>
#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

int main(void) {
  int fd = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK, 0);

  struct sockaddr_in addr = {0};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = 0;
  bind(fd, (struct sockaddr *)&addr, sizeof(addr));

  socklen_t len = sizeof(addr);
  getsockname(fd, (struct sockaddr *)&addr, &len);

  sendto(fd, "", 0, 0, (struct sockaddr *)&addr, sizeof(addr));

  struct pollfd pfd = {.fd = fd, .events = POLLIN};
  poll(&pfd, 1, 1000);

  char byte;
  ssize_t n = recvfrom(fd, &byte, sizeof(byte), 0, NULL, NULL);
  if (n < 0)
    perror("recvfrom after poll");
  else
    printf("received datagram length: %zd\n", n);

  close(fd);
}
```

Callgraph and boundary:

Current problematic path:

```text
C sendto(fd, "", 0, ...)
  -> wasix-libc sendto()
  -> Wasmer sock_send_to()
  -> virtual-net UDP socket receives one zero-length datagram

C poll([{ fd, POLLIN }])
  -> wasix-libc poll()
  -> __wasi_poll_oneoff()
  -> Wasmer lib/wasix/src/syscalls/wasi/poll_oneoff.rs
  -> Wasmer socket readiness path
  -> virtual-net LocalUdpSocket/RemoteUdpSocket readiness
     -> may call recv_from()/peek-like logic against the real socket
        HERE IS THE PROBLEM: readiness can consume or lose the datagram

C recvfrom(fd, ...)
  -> wasix-libc recvfrom()
  -> Wasmer sock_recv_from()
  -> Wasmer lib/wasix/src/net/socket.rs InodeSocket::recv_from(...)
  -> virtual-net try_recv_from(...)
     -> datagram is already gone
        HERE IS THE PROBLEM: guest sees EAGAIN, timeout, or missing packet
```

Proposed solution:

Store one complete UDP datagram in a per-socket backlog when readiness needs to
observe the socket. `poll_read_ready()` should answer from backlog first. A real
guest receive should pop from backlog; a guest peek should copy from backlog
without popping.

Relevant Wasmer code paths:

```text
lib/wasix/src/syscalls/wasi/poll_oneoff.rs
  poll_oneoff_internal(...)
    reports fd-read readiness without consuming guest-visible data

lib/wasix/src/net/socket.rs
  InodeSocket::recv_from(...)
    read from UDP backlog first
    only pop backlog on non-peek receive

lib/virtual-net/src/host.rs
  LocalUdpSocket::try_recv_from(...)
  LocalUdpSocket readiness / poll_read_ready path
    fill backlog once, do not throw datagram away

lib/virtual-net/src/client.rs
  RemoteUdpSocket::try_recv_from(...)
    mirror host-side backlog behavior for remote virtual networking
```

Proposed callgraph:

```text
C sendto(fd, "", 0, ...)
  -> Wasmer sock_send_to()
  -> virtual-net queues one zero-length UDP datagram

C poll([{ fd, POLLIN }])
  -> Wasmer poll_oneoff_internal(...)
  -> UDP readiness checks backlog
  -> if backlog is empty, receive exactly one datagram into backlog
  -> return FdRead without consuming backlog

C recvfrom(fd, ...)
  -> Wasmer InodeSocket::recv_from(...)
  -> virtual-net returns datagram from backlog
  -> backlog pops only because this is a non-peek receive
  -> guest observes received datagram length 0
```

Sketch:

```rust
struct UdpBacklog {
    packets: VecDeque<(Vec<u8>, SocketAddr)>,
}

fn poll_read_ready(&mut self) -> io::Result<usize> {
    if let Some((packet, _)) = self.backlog.front() {
        return Ok(packet.len());
    }

    let mut tmp = vec![0; max_datagram_size()];
    match self.socket.recv_from(&mut tmp) {
        Ok((n, addr)) => {
            tmp.truncate(n);
            self.backlog.push_back((tmp, addr));
            Ok(n)
        }
        Err(err) => Err(err),
    }
}

fn recv_from(&mut self, out: &mut [u8], peek: bool) -> io::Result<(usize, SocketAddr)> {
    if let Some((packet, addr)) = self.backlog.front() {
        let n = out.len().min(packet.len());
        out[..n].copy_from_slice(&packet[..n]);
        let addr = *addr;
        if !peek {
            self.backlog.pop_front();
        }
        return Ok((n, addr));
    }

    if peek {
        self.socket.peek_from(out)
    } else {
        self.socket.recv_from(out)
    }
}
```

Do this for both host and client virtual-net socket implementations. Keep
datagram boundaries intact; do not mix backlog plus a fresh receive into one
guest buffer.

## Proposed Solution References

### [wasmerio/wasmer#6685: Udp datagram receive and last err](https://github.com/wasmerio/wasmer/pull/6685)

- Sadhbh: wasmer [c56897f3bec](https://github.com/Anodized-Titanium/wasmer/commit/c56897f3beced2c9c4861137bf81f32320a14b7c) Fixed UDP socket dgram receive
- Sadhbh: wasmer [65e0eb571ef](https://github.com/Anodized-Titanium/wasmer/commit/65e0eb571ef4fde1aee765fae2955956dcbd1d8f) Peek UDP packets correctly
- Sadhbh: wasmer [66552d27be9](https://github.com/Anodized-Titanium/wasmer/commit/66552d27be9a7efc7962aab22bd60375476c07bb) Fix merge conflict mistake
- Sadhbh: wasmer [8e92612e917](https://github.com/Anodized-Titanium/wasmer/commit/8e92612e917cb9745f21aab2634dd7687c229d33) Conflict resolution for UDP receive/readiness changes in `lib/virtual-net/src/{client,host,lib}.rs` and `lib/wasix/src/net/socket.rs`

