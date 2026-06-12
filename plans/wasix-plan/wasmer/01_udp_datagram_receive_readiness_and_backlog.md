# UDP Datagram Receive, Readiness, and Backlog

Why this is a problem:

UDP readiness is datagram-based, not byte-stream-based. A readiness probe must
not consume a packet that the guest later expects to receive. If `poll_oneoff()`
or the virtual socket readiness path internally calls a destructive receive, the
runtime can report `FdRead` and then make the guest's later `recvfrom()` see
`EAGAIN` or block forever.

When it occurs:

- zero-length UDP packets;
- recursive UDP send/receive callbacks;
- default-host dgram tests;
- any path where libuv polls first and then drains the socket.

Minimal manifestation:

```js
const dgram = require('node:dgram');
const s = dgram.createSocket('udp4');

s.on('message', (msg) => {
  console.log('received', msg.length);
  s.close();
});

s.bind(0, () => {
  const port = s.address().port;
  s.send(Buffer.alloc(0), port, '127.0.0.1');
});
```

Expected call path:

```text
JS dgram.send()
  -> libuv uv_udp_send()
  -> wasix-libc sendto()/sendmsg()
  -> Wasmer sock_send_to()
  -> virtual-net UDP socket

libuv poll
  -> wasix-libc poll_oneoff()
  -> Wasmer poll_oneoff()
  -> socket readiness
  -> later sock_recv_from()
```

Proposed solution:

Store one complete UDP datagram in a per-socket backlog when readiness needs to
observe the socket. `poll_read_ready()` should answer from backlog first. A real
guest receive should pop from backlog; a guest peek should copy from backlog
without popping.

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

Proposed solution can be found in:

**Reference commits:**

```text
wasmer c56897f3bec Fixed UDP socket dgram receive
wasmer 65e0eb571ef Peek UDP packets correctly
wasmer 66552d27be9 Fix merge conflict mistake
wasmer 8e92612e917 Merge branch 'main' into udp-datagram-receive-and-last-err
wasmer bee1e0ab8ae Merge branch 'udp-datagram-receive-and-last-err' into tmp-work-4
```
