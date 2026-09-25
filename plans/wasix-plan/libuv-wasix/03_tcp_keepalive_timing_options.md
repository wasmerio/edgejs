# WASIX TCP keepalive timing options

## Current Edge behavior

Node and Undici request TCP keepalive during HTTP/HTTPS client setup. Libuv
sets `SO_KEEPALIVE` and then `TCP_KEEPIDLE`, `TCP_KEEPINTVL`, and `TCP_KEEPCNT`.
The stock WASIX libc supports the boolean flag but rejects the timing options
with `ENOSYS`. If keepalive was requested before a socket existed, libuv
reapplies those options when opening it and can fail connection establishment.

Passing zero for the delay is not an escape hatch: libuv rejects it for a live
socket and uses a hardcoded delay of 60 when opening a deferred socket.

Edge's native `TcpSetKeepAlive` binding therefore takes a WASIX-only path:

1. Obtain the descriptor with the public `uv_fileno()` API.
2. If it does not exist yet, return success. Unchanged `lib/net.js` caches the
   preference and reapplies it after connect, before emitting `connect`.
3. Set only `SO_KEEPALIVE` through the existing libc mapping and return any
   real socket-option error. Do not set libuv's deferred keepalive flag.

The requested initial delay is ignored on WASIX; probe timing follows the
network backend's defaults. Other platforms still use `uv_tcp_keepalive()`
with the requested delay. Pi, Undici, and Edge's JavaScript library files are
unchanged. HTTP connection reuse and streaming remain available.

This path builds with the existing CI sysroot (`v2026-07-30.1`). It does not
require a new libc ABI, TCP timing support in Wasmer, or a new WISP extension.
The existing browser WISP transport treats the basic flag as a no-op; it does
not promise operating-system TCP probes. Native backends can enable real
keepalive using their existing boolean-option support.

## Verification

`tests/js/wasix-tcp-keepalive.js` exercises requests made before connect, while
connecting, through connect options, and on accepted sockets. It checks native
return values when enabling/disabling with zero and nonzero delays, then
exchanges data. Set `PORT` when the test host does not assign guest listener
ports dynamically.

The stock-sysroot Edge build and import validation pass. The focused test
passes on the SDK Node host. The complete Pi browser test checks installation,
streaming, all seven tools, lock heartbeats, saved sessions, and interactive
startup/exit with the original WISP transport. Long-idle provider connections
are outside this coverage.

## Related work

- [libuv #15](https://github.com/wasix-org/libuv/pull/15) contains only the
  filesystem timestamp fixes; its TCP implementation is unchanged.
- Full configurable probe timing is separate work in
  [WITX #10](https://github.com/wasix-org/wasix-witx/pull/10),
  [libc #140](https://github.com/wasix-org/wasix-libc/pull/140), and
  [Wasmer #7035](https://github.com/wasmerio/wasmer/pull/7035).
  Those keepalive changes are not prerequisites for this Edge fallback.
- Pi still needs the Wasmer/libuv timestamp fixes and
  [N-API #76](https://github.com/wasmerio/napi/pull/76) for host module loading.
