# Connected UDP Send, Peer State, and `EMSGSIZE`

Why this is a problem:

Connected UDP is still UDP. A vectored send represents one datagram, not one
datagram per iovec. The runtime also needs to remember the peer chosen by
`connect()` so `getpeername()` and connected sends behave normally.

When it occurs:

- `test-dgram-connect-send-multi-string-array`;
- `test-dgram-connect-send-multi-buffer-copy`;
- `test-dgram-msgsize`;
- c-ares connected UDP resolver paths.

Minimal manifestation:

```js
const dgram = require('node:dgram');
const s = dgram.createSocket('udp4');

s.bind(0, () => {
  s.connect(s.address().port, '127.0.0.1', () => {
    s.send(['he', 'llo']); // one UDP datagram: "hello"
  });
});
```

Callgraph and boundary:

```text
Node dgram array send
  -> libuv uv_udp_send()
  -> wasix-libc sendmsg()/send()
  -> Wasmer connected UDP send
  -> virtual-net send_to(peer, coalesced_iovs)
```

Proposed solution:

- During UDP `connect()`, preserve the upgraded/auto-bound socket.
- Store the connected peer in socket state.
- For connected UDP vectored send, concatenate iovecs into a single datagram.
- Map oversized datagrams to `Errno::Msgsize`, not a generic I/O error.

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

Proposed solution can be found in:

**Reference commits:**

```text
wasmer 50e5c1f1375 connected UDP send() now coalesces iovecs into one datagram...
wasmer dc9e005159a WASIX UDP connect() now preserves the auto-bound UDP socket...
```
