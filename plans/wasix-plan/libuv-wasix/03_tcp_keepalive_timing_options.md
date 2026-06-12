# TCP Keepalive Timing Options

Why this is a problem:

Node and Undici call `uv_tcp_keepalive()`. On native platforms, libuv may set
`SO_KEEPALIVE` and then tune `TCP_KEEPIDLE`, `TCP_KEEPINTVL`, and
`TCP_KEEPCNT`. WASIX may accept `SO_KEEPALIVE` but reject the TCP timing knobs.
That turns harmless keepalive setup into a failed fetch or HTTP client setup.

Minimal manifestation:

```js
await fetch('http://127.0.0.1:12345/');
```

Boundary:

```text
Undici fetch()
  -> EdgeJS TCPWrap
  -> libuv uv_tcp_keepalive()
  -> setsockopt(SO_KEEPALIVE)
  -> setsockopt(IPPROTO_TCP, TCP_KEEPIDLE/INTVL/KEEPCNT)
```

Proposed solution:

Until WASIX supports the TCP keepalive tuning knobs, make libuv's WASIX path
accept the keepalive request without issuing unsupported timing options. The
longer-term POSIX-complete answer is to implement these socket options in
`wasix-libc` and Wasmer.

Sketch:

```c
#if defined(__wasi__) || defined(__wasm32__)
  return 0;
#else
  return uv__tcp_keepalive(fd, on, delay);
#endif
```

## Proposed Solution References

Proposed solution can be found in:

**Reference commits:**

```text
edgejs 9fa61f18 UV keep alive fix (disable unsupported options)
```
