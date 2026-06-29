# Connected UDP Peer State and `EMSGSIZE`

Why this is a problem:

In the baseline runtime, connected UDP can lose POSIX socket state after
`connect()`.

`connect()` can auto-bind or upgrade a UDP pre-socket, but the resulting Wasmer
socket state does not reliably keep the concrete connected socket and its peer
address together. After that, `getpeername()` can report `ENOTCONN` or an empty
peer even though `connect()` succeeded, and connected sends may run through a
path that no longer knows which peer should receive the datagram.

Oversized UDP sends have a related error-mapping problem. Host networking can
report a datagram as too large, but if the virtual network layer maps that to a
generic I/O error, JavaScript and POSIX callers see the wrong error class. The
runtime should preserve `EMSGSIZE` / `Errno::Msgsize`.

The fix makes Wasmer preserve the connected UDP peer after `connect()`, make
`getpeername()` read that stored peer, route connected `send()` through that
peer, and map oversized datagrams to `EMSGSIZE` / `Errno::Msgsize` rather than a
generic I/O error.

When it occurs:

- `test-dgram-connect-send-*` rows that use connected UDP;
- `test-dgram-msgsize`;
- c-ares connected UDP resolver paths.

Minimal Example:

```c
#include <arpa/inet.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

int main(void) {
  int receiver = socket(AF_INET, SOCK_DGRAM, 0);
  int sender = socket(AF_INET, SOCK_DGRAM, 0);

  struct sockaddr_in addr = {0};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = 0;
  bind(receiver, (struct sockaddr *)&addr, sizeof(addr));

  socklen_t len = sizeof(addr);
  getsockname(receiver, (struct sockaddr *)&addr, &len);
  connect(sender, (struct sockaddr *)&addr, sizeof(addr));

  struct sockaddr_in peer = {0};
  socklen_t peer_len = sizeof(peer);
  getpeername(sender, (struct sockaddr *)&peer, &peer_len);
  printf("connected UDP peer port: %u\n", ntohs(peer.sin_port));

  send(sender, "hello", 5, 0);

  char buf[16] = {0};
  struct sockaddr_in peer_from = {0};
  socklen_t peer_from_len = sizeof(peer_from);
  ssize_t n = recvfrom(receiver, buf, sizeof(buf), 0,
                       (struct sockaddr *)&peer_from, &peer_from_len);
  printf("received %zd bytes from %s:%u: %.*s\n", n,
         inet_ntoa(peer_from.sin_addr), ntohs(peer_from.sin_port), (int)n, buf);

  close(sender);
  close(receiver);
}
```

Callgraph and boundary:

Current problematic path:

```text
C connect(sender, peer)
  -> wasix-libc connect()
  -> Wasmer lib/wasix/src/net/socket.rs InodeSocket::connect(...)
     -> for an existing UdpSocket, peer may be stored as `peer: Option<SocketAddr>`
     -> for an auto-bound/upgraded UDP socket, the concrete socket can be lost
        or the old pre-socket can remain visible
        HERE IS THE PROBLEM: connected UDP state is split or missing

C getpeername(sender)
  -> wasix-libc getpeername()/sock_addr_peer
  -> Wasmer lib/wasix/src/net/socket.rs InodeSocket::addr_peer()
     -> may see no stored UDP peer
     -> may fall through to virtual-net addr_peer() returning None/NotConnected
        HERE IS THE PROBLEM: getpeername() can report ENOTCONN after connect()

C send(sender, "hello", 5, 0)
  -> wasix-libc send()
  -> Wasmer lib/wasix/src/syscalls/wasix/sock_send.rs
  -> Wasmer lib/wasix/src/net/socket.rs InodeSocket::send(...)
     -> may not have a stored UDP peer
        HERE IS THE PROBLEM: connected send can fail as NotConnected
```

Proposed solution:

- During UDP `connect()`, if the runtime upgrades or auto-binds the socket,
  keep the upgraded socket entry instead of falling back to the old pre-socket.
- Store the connected peer address in Wasmer socket state.
- Implement `addr_peer()` / `getpeername()` for connected UDP by returning the
  stored peer.
- Route connected UDP `send()` to the stored peer.
- Map oversized datagrams to `Errno::Msgsize`, not a generic I/O error.

Relevant Wasmer code paths:

```text
lib/wasix/src/net/socket.rs
  InodeSocket::connect(...)
    preserve the concrete UDP socket and set `peer: Some(peer)`

  InodeSocket::addr_peer(...)
    for InodeSocketKind::UdpSocket, return the stored peer first

  InodeSocket::send(...)
    for connected InodeSocketKind::UdpSocket, send to stored peer
    map NetworkError::MessageSize to Errno::Msgsize

lib/virtual-net/src/host.rs
  LocalUdpSocket::addr_peer(...)
  LocalUdpSocket::try_send_to(...)

lib/virtual-net/src/client.rs
  RemoteUdpSocket::addr_peer(...)
  RemoteUdpSocket::try_send_to(...)
```

Proposed callgraph:

```text
C connect(sender, peer)
  -> wasix-libc connect()
  -> Wasmer InodeSocket::connect(...)
     -> keep upgraded/bound UDP socket
     -> set socket peer = Some(peer)

C getpeername(sender)
  -> wasix-libc getpeername()/sock_addr_peer
  -> Wasmer InodeSocket::addr_peer()
     -> return socket peer = Some(peer)

C send(sender, "hello", 5, 0)
  -> wasix-libc send()
  -> Wasmer sock_send_internal()
  -> Wasmer InodeSocket::send(...)
     -> virtual-net try_send_to("hello", peer)
     -> receiver gets one datagram from the connected sender
```

Sketch:

```rust
fn udp_connect(socket: InodeSocket, peer: SocketAddr) -> Result<Option<InodeSocket>, Errno> {
    let bound_socket = socket.auto_bind_udp(tasks, net).await?;
    let socket = bound_socket.clone().unwrap_or(socket);
    let connected_socket = socket.connect_udp_in_place(peer)?;
    Ok(connected_socket.or(bound_socket))
}

fn udp_connected_send(buf: &[u8], peer: SocketAddr) -> Result<usize, Errno> {
    socket
        .send_to(buf, peer)
        .map_err(|err| match err {
            NetworkError::MessageSize => Errno::Msgsize,
            other => map_socket_err(other),
        })
}
```

## Proposed Solution References

### [wasmerio/wasmer#6685: Udp datagram receive and last err](https://github.com/wasmerio/wasmer/pull/6685)

- Sadhbh: wasmer [50e5c1f1375](https://github.com/Anodized-Titanium/wasmer/commit/50e5c1f137523e683e1b52a8f847dcdc35ca0b37) Connected UDP peer state and `EMSGSIZE` mapping.
- Sadhbh: wasmer [dc9e005159a](https://github.com/Anodized-Titanium/wasmer/commit/dc9e005159ad7be200d9daa3e16e8da775cf0f9e) WASIX UDP connect() now preserves the auto-bound UDP socket.
