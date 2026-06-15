# EdgeJS: c-ares request lifecycle and address-family selection

Why this is a problem:

EdgeJS owns the JavaScript-facing DNS binding around c-ares. If a resolver
channel is cancelled or destroyed while requests are still active, Node expects
those requests to complete with a cancellation error. If EdgeJS only tears down
the c-ares channel, the JavaScript callback can be lost and the test waits until
its timeout. Address-family selection has the same shape: JavaScript asks for
IPv4 or IPv6, but if the binding does not propagate that choice into c-ares, the
wrong family can be returned and downstream UDP/TCP tests start failing for the
wrong reason.

This occurs in DNS tests that cancel c-ares work, use several resolver channels,
perform reverse lookups, or ask for a specific address family.

Why native EdgeJS passes, and why WASIX still needs this:

Native EdgeJS usually runs on top of mature OS networking, native c-ares timing,
and the V8/Node execution profile that the binding was first exercised against.
Those conditions can make cancellation races and address-family mistakes much
harder to see: DNS work completes quickly, channel teardown ordering differs, and
IPv4/IPv6 answers often come from the host resolver in an order that happens to
match the test. Under WASIX, c-ares is running through libuv, `wasix-libc`, and
Wasmer socket/poll behavior, so cancellation and resolver completion ordering is
more deterministic and exposed the missing EdgeJS-owned request bookkeeping. The
needed EdgeJS change is not a WASIX socket workaround; it is making the c-ares
binding obey the Node contract even when the WASIX runtime schedules DNS work
differently from native.

Minimal Example:

```js
const assert = require('node:assert');
const dns = require('node:dns');

const resolver = new dns.Resolver();
let called = false;
resolver.resolve4('example.invalid', (err) => {
  called = true;
  assert(err);
  assert.strictEqual(err.code, 'ECANCELLED');
});
resolver.cancel();

setTimeout(() => {
  assert.strictEqual(called, true);
}, 10);

dns.lookup('localhost', { family: 4 }, (err, address, family) => {
  assert.ifError(err);
  assert.strictEqual(family, 4);
});
```

Failing tests that showed this class:

```text
parallel/test-c-ares
parallel/test-dns-cancel-reverse-lookup
parallel/test-dns-channel-cancel
parallel/test-dns-channel-cancel-promise
parallel/test-dns-multi-channel
parallel/test-dns-perf_hooks
parallel/test-dns-resolveany
parallel/test-dns-resolver-max-timeout
parallel/test-dns-setserver-when-querying
parallel/test-dns
```

Callgraph and boundary:

Current problematic path:

```text
JavaScript dns.Resolver.resolve4()
  -> EdgeJS src/edge_cares_wrap.cc creates a request wrapper
  -> c-ares owns the in-flight query

JavaScript resolver.cancel()
  -> EdgeJS cancels/destroys c-ares channel
     HERE IS THE PROBLEM: active EdgeJS request wrappers may not all be completed
     with ECANCELLED, so JavaScript mustCall callbacks are never reached.

JavaScript dns.lookup(..., { family: 4 })
  -> EdgeJS GetAddrInfo/GetNameInfo wrapper
     HERE IS THE PROBLEM: requested family can be lost or loosened before the
     c-ares query, so IPv6 can leak into IPv4-only paths or the reverse.
```

The boundary is EdgeJS <-> c-ares. This is not a Wasmer socket fix: the runtime
can deliver UDP packets correctly and the binding can still lose request
completion state.

Proposed solution:

Keep this fix in EdgeJS because the binding owns c-ares request lifetime and the
Node-visible DNS callback contract.

Relevant EdgeJS code paths:

```text
~/src/edgejs/src/edge_cares_wrap.cc
~/src/edgejs/test/parallel/test-dns-*.js
~/src/edgejs/test/parallel/test-c-ares.js
```

Proposed callgraph:

```text
JavaScript resolver.cancel()
  -> EdgeJS channel cancel
  -> EdgeJS iterates the channel's active request set
  -> each still-active request completes exactly once with ECANCELLED
  -> request is removed from the active set
  -> JavaScript callback/promise observes the expected cancellation

JavaScript dns.lookup(..., { family })
  -> EdgeJS parses requested family
  -> EdgeJS passes matching c-ares hints/query type
  -> JavaScript receives an address with the requested family
```

The minimal implementation is to make each c-ares channel own an explicit set of
active EdgeJS request objects, complete still-active requests during cancellation,
and preserve the requested address family when constructing c-ares work.

## Proposed Solution References

### [wasmerio/edgejs#91: [WIP] Node tests using Edgejs WASIX QuickJS](https://github.com/wasmerio/edgejs/pull/91)

- Sadhbh: edgejs [929a9151](https://github.com/wasmerio/edgejs/commit/929a9151c336b5862839d7b3d3e2bec219f852ca) c-ares requests are now tracked per channel, cancellation completes still-active requests with ECANCELLED, reverse lookup includes h_name, and loopback reverse/nameinfo returns localhost
- Sadhbh: edgejs [64faa6ca](https://github.com/wasmerio/edgejs/commit/64faa6caba3648b7393c69c9a339a4b90f2a697e) Respect address family (IPv4 or IPv6) when asked GetAddrInfo
