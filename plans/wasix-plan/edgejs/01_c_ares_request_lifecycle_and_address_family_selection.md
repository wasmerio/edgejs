# c-ares Request Lifecycle and Address Family Selection

Why this is a problem:

The c-ares binding owns request lifecycle. If a resolver channel is cancelled,
still-active requests must complete with cancellation rather than vanishing.
When callers request IPv4 or IPv6, EdgeJS must pass that family through
consistently.

When it occurs:

- `test-dns-channel-cancel`;
- `test-dns-channel-cancel-promise`;
- `test-dns-cancel-reverse-lookup`;
- `test-dns-multi-channel`;
- `test-http-autoselectfamily`.

Minimal manifestation:

```js
const dns = require('node:dns');
const r = new dns.Resolver();

r.resolve4('example.com', (err) => console.log(err && err.code));
r.cancel(); // callback must still run with ECANCELLED
```

Boundary:

```text
Node dns.Resolver
  -> EdgeJS c-ares binding
  -> c-ares channel/request objects
  -> UDP sockets via libuv/wasix-libc/Wasmer
```

Proposed solution:

Track active requests by c-ares channel. On cancellation, complete pending
requests with `ECANCELLED`. Respect the family requested by JavaScript when
building `GetAddrInfo` hints.

Sketch:

```cpp
struct ChannelState {
  std::unordered_set<Request*> active;
};

void Cancel(ChannelState* channel) {
  for (Request* req : channel->active)
    req->Complete(ARES_ECANCELLED);
  channel->active.clear();
}
```

## Proposed Solution References

Proposed solution can be found in:

**Reference commits:**

```text
edgejs 929a9151 c-ares requests are now tracked per channel...
edgejs 64faa6ca Respect address family (IPv4 or IPv6) when asked GetAddrInfo
```
