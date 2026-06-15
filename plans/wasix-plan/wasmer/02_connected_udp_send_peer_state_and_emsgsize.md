# Connected UDP Send, Peer State, and `EMSGSIZE`

Why this is a problem:

In the baseline runtime, connected UDP loses two pieces of POSIX socket
semantics.

First, `connect()` can auto-bind or upgrade a UDP pre-socket, but the resulting
Wasmer socket state does not reliably keep the concrete connected socket and its
peer address together. After that, `getpeername()` can report `ENOTCONN` or an
empty peer even though `connect()` succeeded, and connected sends may run through
a path that no longer knows which peer should receive the datagram.

Second, vectored writes to a connected UDP socket can be treated like stream
writes: each iovec can be sent separately. That is wrong for UDP. A single
`writev()` / `sendmsg()` call on a connected UDP socket represents one datagram
containing the concatenated iovec bytes. If the runtime sends one datagram per
iovec, the receiver observes `"he"` and `"llo"` as separate packets instead of
one `"hello"` packet.

The fix makes Wasmer preserve the connected UDP peer after `connect()`, make
`getpeername()` read that stored peer, and make connected vectored UDP send
coalesce all iovecs into one datagram before calling the virtual network layer.
Oversized coalesced packets should fail as `EMSGSIZE` / `Errno::Msgsize`, not as
a generic I/O error.

When it occurs:

- `test-dgram-connect-send-multi-string-array`;
- `test-dgram-connect-send-multi-buffer-copy`;
- `test-dgram-msgsize`;
- c-ares connected UDP resolver paths.

Minimal Example:

```c
#include <arpa/inet.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/uio.h>
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

  struct iovec iov[2] = {
    {.iov_base = "he", .iov_len = 2},
    {.iov_base = "llo", .iov_len = 3},
  };
  writev(sender, iov, 2);

  char buf[16] = {0};
  ssize_t n = recvfrom(receiver, buf, sizeof(buf), 0, NULL, NULL);
  printf("received %zd bytes: %.*s\n", n, (int)n, buf);

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

C writev(sender, ["he", "llo"])
  -> wasix-libc writev()/sendmsg-shaped path
  -> Wasmer connected UDP send path
     -> each iovec can be treated like an independent send buffer
     -> virtual-net try_send_to("he", peer)
     -> virtual-net try_send_to("llo", peer)
        HERE IS THE PROBLEM: receiver observes two UDP datagrams, not one
```

Proposed solution:

- During UDP `connect()`, if the runtime upgrades or auto-binds the socket,
  keep the upgraded socket entry instead of falling back to the old pre-socket.
- Store the connected peer address in Wasmer socket state.
- Implement `addr_peer()` / `getpeername()` for connected UDP by returning the
  stored peer.
- For connected UDP vectored send, concatenate iovecs into one temporary packet
  and send exactly one datagram to the stored peer.
- Map oversized datagrams to `Errno::Msgsize`, not a generic I/O error.

Relevant Wasmer code paths:

```text
lib/wasix/src/net/socket.rs
  InodeSocket::connect(...)
    preserve the concrete UDP socket and set `peer: Some(peer)`

  InodeSocket::addr_peer(...)
    for InodeSocketKind::UdpSocket, return the stored peer first

  connected UDP send path / sock_send_msg path
    coalesce iovecs before calling virtual-net
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

C writev(sender, ["he", "llo"])
  -> wasix-libc writev()/sendmsg-shaped path
  -> Wasmer connected UDP send path
     -> coalesce iovecs into "hello"
     -> virtual-net try_send_to("hello", peer)
     -> receiver gets exactly one datagram
```

Sketch:

```rust
fn udp_connected_send(iovs: &[IoSlice<'_>], peer: SocketAddr) -> Result<usize, Errno> {
    let len: usize = iovs.iter().map(|iov| iov.len()).sum();
    if len > max_udp_payload() {
        return Err(Errno::Msgsize);
    }

    let mut packet = Vec::with_capacity(len);
    for iov in iovs {
        packet.extend_from_slice(iov);
    }

    socket.send_to(&packet, peer).map_err(map_socket_err)?;
    Ok(len)
}
```

## Proposed Solution References

### [wasmerio/wasmer#6685: Udp datagram receive and last err](https://github.com/wasmerio/wasmer/pull/6685)

- wasmer [50e5c1f1375](https://github.com/Anodized-Titanium/wasmer/commit/50e5c1f137523e683e1b52a8f847dcdc35ca0b37) connected UDP send() now coalesces iovecs into one datagram...
- wasmer [dc9e005159a](https://github.com/Anodized-Titanium/wasmer/commit/dc9e005159ad7be200d9daa3e16e8da775cf0f9e) WASIX UDP connect() now preserves the auto-bound UDP socket...
