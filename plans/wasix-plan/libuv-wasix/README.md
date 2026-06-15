# libuv-wasix Plan

- [Use POSIX Spawn on WASIX](01_use_posix_spawn_on_wasix.md)
- [UDP Disconnect and Multicast Option Compatibility](02_udp_disconnect_and_multicast_option_compatibility.md)
- [TCP Keepalive Timing Options](03_tcp_keepalive_timing_options.md)

## Branch Context

For `libuv-wasix`, the mainline branch is named `ubi`, not `main`. The local
`fix/spawn` branch is currently layered as:

```text
Sadhbh 0ae770b9 multicast TTL/loop/interface shims for WASIX, plus WASIX UDP disconnect avoids the unsupported AF_UNSPEC connect() trick. WASIX connected-state checks now trust libuv's handle flag.
Artem  ba06698b libuv-wasix: use WASIX posix_spawn/proc_join for child processes
Martin ea792e22 disable fork   # origin/ubi, ubi baseline
```
