# libuv-wasix: TCP keepalive timing options

Why this is a problem:

Node and Undici call `uv_tcp_keepalive()` as part of normal HTTP/HTTPS client
setup. On Unix, libuv enables `SO_KEEPALIVE` and then often configures TCP
keepalive timing knobs such as `TCP_KEEPIDLE`, `TCP_KEEPINTVL`, and
`TCP_KEEPCNT`. WASIX may accept `SO_KEEPALIVE` but reject the timing knobs. If
libuv treats those timing failures as fatal, ordinary `fetch()` and keepalive
HTTP requests fail even though the socket itself is usable.

This occurs in fetch/HTTP/HTTPS client tests that configure keepalive.

Minimal Example:

```c
#include <uv.h>

int main(void) {
  uv_loop_t *loop = uv_default_loop();
  uv_tcp_t tcp;
  uv_tcp_init(loop, &tcp);
  return uv_tcp_keepalive(&tcp, 1, 60) == 0 ? 0 : 1;
}
```

Callgraph and boundary:

Current problematic path:

```text
JavaScript fetch()/HTTP client
  -> Node TCPWrap::SetKeepAlive
  -> libuv uv_tcp_keepalive()
  -> libuv uv__tcp_keepalive()
  -> setsockopt(SOL_SOCKET, SO_KEEPALIVE)
  -> setsockopt(IPPROTO_TCP, TCP_KEEPIDLE/TCP_KEEPINTVL/TCP_KEEPCNT)
     HERE IS THE PROBLEM: WASIX rejects timing knobs, and libuv propagates that
     as failure for the whole keepalive operation.
```

The boundary is libuv <-> socket options. Long-term, `wasix-libc`/Wasmer should
support the useful TCP options where possible. Until then, libuv should not make
unsupported timing knobs break basic keepalive enablement.

Proposed solution:

Keep EdgeJS calling `uv_tcp_keepalive()` normally. In libuv-wasix, treat the
WASIX timing knobs as optional when `SO_KEEPALIVE` has been applied or when the
platform cannot expose those knobs yet.

Relevant libuv-wasix code paths:

```text
~/src/edgejs/deps/libuv-wasix/src/unix/tcp.c
~/src/edgejs/deps/libuv-wasix/src/unix/internal.h
~/src/wasix-libc/libc-bottom-half/cloudlibc/src/libc/sys/socket/setsockopt.c
~/src/wasmer/lib/wasix/src/syscalls/wasix/sock_set_opt_*.rs
```

Proposed callgraph:

```text
JavaScript fetch()/HTTP client
  -> Node TCPWrap::SetKeepAlive
  -> libuv uv_tcp_keepalive()
  -> WASIX uv__tcp_keepalive()
  -> enable/disable SO_KEEPALIVE when available
  -> skip or tolerate unsupported TCP timing knobs
  -> return success for the keepalive operation
```

This is deliberately small: make basic Node keepalive setup succeed without
claiming that WASIX already implements every TCP timing option.

## Proposed Solution References

### [wasmerio/edgejs#91: [WIP] Node tests using Edgejs WASIX QuickJS](https://github.com/wasmerio/edgejs/pull/91)

- Sadhbh: edgejs [9fa61f18](https://github.com/wasmerio/edgejs/commit/9fa61f1888f34c0785f54da8dd99ae193e536439) UV keep alive fix (disable unsupported options)

### Commits without PR

- Sadhbh: libuv-wasix [8d537440](https://github.com/Anodized-Titanium/libuv-wasix/commit/8d537440533cfc290e33c7bcbf181ab414dd1850) Wasix-LibC supports SOL_SOCKET + SO_KEEPALIVE, however does not support additional options such as TCP_KEEPIDLE, TCP_KEEPINTVL, or TCP_KEEPCNT - so we disable them
