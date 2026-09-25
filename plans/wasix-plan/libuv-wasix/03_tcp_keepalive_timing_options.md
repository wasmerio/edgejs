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

The missing support belongs in `wasix-libc` and Wasmer's socket options.
Libuv should configure the requested values and propagate failures normally.

Runtime solution:

Keep EdgeJS and libuv calling `uv_tcp_keepalive()` and `setsockopt()` normally.
Map the three TCP timing options through the existing WASIX socket-option
imports, retain settings made before socket connection, and apply them through
Wasmer's networking backend. Unsupported options and invalid values remain
errors.

Relevant libuv-wasix code paths:

```text
~/src/edgejs/deps/libuv-wasix/src/unix/tcp.c
~/src/edgejs/deps/libuv-wasix/src/unix/internal.h
~/src/wasix-libc/libc-bottom-half/cloudlibc/src/libc/sys/socket/setsockopt.c
~/src/wasmer/lib/wasix/src/syscalls/wasix/sock_set_opt_*.rs
```

Implemented callgraph:

```text
JavaScript fetch()/HTTP client
  -> Node TCPWrap::SetKeepAlive
  -> libuv uv_tcp_keepalive()
  -> libuv uv__tcp_keepalive()
  -> libc setsockopt(IPPROTO_TCP, TCP_KEEP*)
  -> WASIX sock_set_opt_size(TcpKeepIdle/TcpKeepInterval/TcpKeepCount)
  -> Wasmer socket state and networking backend
  -> apply requested settings or return the backend error
```

## Earlier workaround references (superseded)

### [wasmerio/edgejs#91: [WIP] Node tests using Edgejs WASIX QuickJS](https://github.com/wasmerio/edgejs/pull/91)

- Sadhbh: edgejs [9fa61f18](https://github.com/wasmerio/edgejs/commit/9fa61f1888f34c0785f54da8dd99ae193e536439) UV keep alive fix (disable unsupported options)

### Commits without PR

- Sadhbh: libuv-wasix [8d537440](https://github.com/Anodized-Titanium/libuv-wasix/commit/8d537440533cfc290e33c7bcbf181ab414dd1850) Wasix-LibC supports SOL_SOCKET + SO_KEEPALIVE, however does not support additional options such as TCP_KEEPIDLE, TCP_KEEPINTVL, or TCP_KEEPCNT - so we disable them

## September 2026 implementation

[libuv PR #15](https://github.com/wasix-org/libuv/pull/15) is limited to
filesystem timestamp fixes and tests. Its TCP error-suppression wrapper has
been removed; `src/unix/tcp.c` matches the upstream WASIX branch.

- [WITX #10](https://github.com/wasix-org/wasix-witx/pull/10) appends socket
  option IDs 27, 28, and 29 for idle seconds, interval seconds, and probe count.
  Existing option IDs and syscall signatures remain unchanged.
- [wasix-libc #140](https://github.com/wasix-org/wasix-libc/pull/140) maps the
  POSIX TCP options to the existing size-option imports, with positive integer
  validation and matching getters.
- [Wasmer #7035](https://github.com/wasmerio/wasmer/pull/7035) preserves the
  settings through socket creation and connection and implements the backend
  options, alongside the filesystem timestamp fixes.

An Edge.js WASIX artifact must be rebuilt against a sysroot containing the
libc change and run with the updated Wasmer networking implementation. Local
builds can use the existing override:

```sh
WASIXCC_SYSROOT=/path/to/rebuilt/sysroot ./wasix/build-wasix.sh
```

Both WASIX CI workflows currently pin sysroot `v2026-07-30.1`, which predates
these changes. A released sysroot containing wasix-libc #140 is still required
before updating that pin and producing release artifacts with this support.
No unreleased tag is assumed here. The local engine-free Edge build, npm/pnpm
staging, and WebC package build passed using the rebuilt libc sysroot.
