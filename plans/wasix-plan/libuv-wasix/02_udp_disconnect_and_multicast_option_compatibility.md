# libuv-wasix: UDP disconnect and multicast option compatibility

Why this is a problem:

libuv's Unix UDP code relies on Linux behavior that is not fully available in
WASIX today. In particular, Linux disconnects a connected UDP socket with
`connect(AF_UNSPEC)`, and libuv exposes multicast option helpers that map to
platform `setsockopt()` calls. If WASIX returns `ENOTSUP`/`ENOSYS` for those
operations and libuv forwards that directly, Node dgram tests fail before they
reach the behavior they are trying to assert.

This occurs in connected UDP, default-host send, multicast TTL/interface/loop,
and cluster/dgram tests.

Branch context:

`libuv-wasix` uses `ubi` as its mainline branch. In the local history, the UDP
and multicast work is Sadhbh's `0ae770b9` on top of Artem's `ba06698b` spawn
commit, with Martin's `ea792e22` `ubi` commit as the baseline:

```text
Sadhbh 0ae770b9 multicast TTL/loop/interface shims for WASIX, plus WASIX UDP disconnect avoids the unsupported AF_UNSPEC connect() trick. WASIX connected-state checks now trust libuv's handle flag.
Artem  ba06698b libuv-wasix: use WASIX posix_spawn/proc_join for child processes
Martin ea792e22 disable fork   # origin/ubi, ubi baseline
```

Minimal Example:

```c
#include <uv.h>
#include <string.h>

int main(void) {
  uv_loop_t *loop = uv_default_loop();
  uv_udp_t udp;
  struct sockaddr_in addr;
  uv_udp_init(loop, &udp);
  uv_ip4_addr("127.0.0.1", 9, &addr);

  if (uv_udp_connect(&udp, (const struct sockaddr *)&addr) != 0) return 1;
  if (uv_udp_connect(&udp, NULL) != 0) return 2; /* disconnect */
  if (uv_udp_set_multicast_ttl(&udp, 1) != 0) return 3;
  return 0;
}
```

Callgraph and boundary:

Current problematic path:

```text
Node dgram socket.disconnect()
  -> libuv uv_udp_connect(handle, NULL)
  -> Unix UDP disconnect path uses connect(AF_UNSPEC)
     HERE IS THE PROBLEM: WASIX does not implement Linux AF_UNSPEC UDP
     disconnect semantics, so libuv reports an operation failure.

Node socket.setMulticastTTL()/setMulticastLoopback()/setMulticastInterface()
  -> libuv multicast helper
  -> setsockopt(multicast option)
     HERE IS THE PROBLEM: some WASIX multicast options are not implemented yet,
     so tests fail at option setup rather than packet behavior.
```

The boundary is libuv <-> POSIX-ish socket API. Long-term multicast packet
semantics belong in Wasmer virtual networking, but libuv still needs a sensible
WASIX compatibility path for options Node expects to call.

Proposed solution:

For UDP disconnect, avoid the Linux-only `connect(AF_UNSPEC)` trick on WASIX and
update libuv's connected state consistently with the WASIX socket behavior.

For multicast option helpers, provide narrow WASIX shims that either succeed
when the option can be represented safely or return a stable, intentional error
for truly unsupported behavior. Do not patch EdgeJS dgram code.

Relevant libuv-wasix code paths:

```text
~/src/edgejs/deps/libuv-wasix/src/unix/udp.c
~/src/edgejs/deps/libuv-wasix/src/uv-common.c
~/src/wasmer/lib/virtual-net/src/host.rs
~/src/wasmer/lib/virtual-net/src/client.rs
```

Proposed callgraph:

```text
Node dgram socket.disconnect()
  -> libuv uv_udp_connect(handle, NULL)
  -> WASIX branch clears libuv connected state without AF_UNSPEC
  -> later sends use unconnected UDP behavior

Node multicast option call
  -> libuv WASIX helper
  -> supported option: update libuv/WASIX-compatible state and return 0
  -> unsupported option: return clear unsupported status without corrupting socket state
```

This keeps the temporary compatibility code in libuv, where Node's dgram API is
translated onto the platform socket API, while the full virtual-network
multicast implementation remains a Wasmer task.

## Proposed Solution References

### Commits without PR

- Sadhbh: libuv-wasix [0ae770b9](https://github.com/Anodized-Titanium/libuv-wasix/commit/0ae770b9daae35d97d3c9a2693a5b22362124f12) multicast TTL/loop/interface shims and UDP disconnect handling
