# WASIX QuickJS Compatibility Fix Plan

| | | Remarks |
| --- | --- | --- |
| **Status** | ▶️ | Forward-looking plan for applying the compatibility work from a GitHub-baseline state. |
| **Severity** | High | These items explain the main gaps between a clean GitHub baseline and the local EdgeJS QuickJS WASIX Node test behavior. |


The detailed plan is split by project. Each project directory contains one file per problem section.

## Baseline Assumption

This plan is written as if the tree starts from the GitHub-style baseline:

- Wasmer is on `main`.
- `wasix-libc` is on `main`.
- QuickJS is on `master`.
- `libuv-wasix` is on its `ubi` branch.
- EdgeJS is on `main` branch.

The commit hashes in headings are reference points only. Treat them as prior art
for the shape and scope of the change, not as statements that the baseline
already contains the behavior.

## Cross-Project Rule

Fix behavior at the lowest POSIX-compatible layer that owns it:

```text
wasix-libc first -> Wasmer second -> libuv-wasix third -> EdgeJS/N-API/QuickJS last
```

EdgeJS should behave like a normal guest. If native EdgeJS works, the WASIX
version should not need EdgeJS-specific workarounds for argv, resolver files,
socket modes, stat modes, or process behavior.


## Project Chapters

- [Wasmer](wasmer/README.md)
- [wasix-libc](wasix-libc/README.md)
- [libuv-wasix](libuv-wasix/README.md)
- [EdgeJS](edgejs/README.md)
- [N-API](napi/README.md)
- [QuickJS](quickjs/README.md)
- [Networking follow-up](networking/README.md)

## Section Files

### Wasmer

- [UDP Datagram Receive, Readiness, and Backlog](wasmer/01_udp_datagram_receive_readiness_and_backlog.md)
- [Connected UDP Peer State and `EMSGSIZE`](wasmer/02_connected_udp_send_peer_state_and_emsgsize.md)
- [UDP Datagram Iovec Boundaries for `writev()` and `sendmsg()`](wasmer/07_udp_datagram_iovec_boundaries_for_writev_and_sendmsg.md)
- [Last Socket Error / `SO_ERROR`](wasmer/03_last_socket_error_so_error.md)
- [`proc_spawn3` / `proc_exec4` for Real argv/envp](wasmer/04_proc_spawn3_proc_exec4_for_real_argv_envp.md)
- [Partial Success for `fd_write`](wasmer/05_partial_success_for_fd_write.md)
- [Future: `sendmsg` / `recvmsg` Control Data and FD Passing](wasmer/06_future_sendmsg_recvmsg_control_data_and_fd_passing.md)

### wasix-libc

- [Socket Type Flags: `SOCK_NONBLOCK` and `SOCK_CLOEXEC`](wasix-libc/01_socket_type_flags_sock_nonblock_and_sock_cloexec.md)
- [Boolean Socket Option Pointer Handling](wasix-libc/02_boolean_socket_option_pointer_handling.md)
- [`getsockopt(SO_ERROR)`](wasix-libc/03_getsockopt_so_error.md)
- [POSIX `st_mode` for Files and Directories](wasix-libc/04_posix_st_mode_for_files_and_directories.md)
- [Loopback Reverse Lookup](wasix-libc/05_loopback_reverse_lookup.md)
- [Child Stdio Pipe `isatty()` Classification](wasix-libc/06_child_stdio_pipe_isatty.md)

### libuv-wasix

- [Use POSIX Spawn on WASIX](libuv-wasix/01_use_posix_spawn_on_wasix.md)
- [UDP Disconnect and Multicast Option Compatibility](libuv-wasix/02_udp_disconnect_and_multicast_option_compatibility.md)
- [TCP Keepalive Timing Options](libuv-wasix/03_tcp_keepalive_timing_options.md)

### EdgeJS

- [c-ares Request Lifecycle and Address Family Selection](edgejs/01_c_ares_request_lifecycle_and_address_family_selection.md)
- [Mount Resolver and Certificate Files Instead of Hard-Coding Answers](edgejs/02_mount_resolver_and_certificate_files_instead_of_hard_coding_answers.md)
- [Remove WASIX-Only Guest Workarounds After Lower-Layer Plans Land](edgejs/03_remove_wasix_only_guest_workarounds_after_lower_layer_plans_land.md)
- [OpenSSL Error Mapping and Version Capability](edgejs/04_openssl_error_mapping_and_version_capability.md)
- [Runner Determinism and Workflow Setup](edgejs/05_runner_determinism_and_workflow_setup.md)

### N-API

- [QuickJS Structured Clone / Serdes for SharedArrayBuffer](napi/01_quickjs_structured_clone_serdes_for_sharedarraybuffer.md)
- [QuickJS/N-API Callsite Frames](napi/02_quickjs_napi_callsite_frames.md)

### QuickJS

- [WASI Stack Limit Should Match Non-WASI QuickJS](quickjs/01_wasi_stack_limit_should_match_non_wasi_quickjs.md)

### Networking

- [Remaining Unsatisfied Networking Plan](networking/01_remaining_unsatisfied_networking_plan.md)
