# Native QuickJS Wasmer host signal interception

| | | Remarks |
| --- | --- | --- |
| **Status** | 🟠 | Diagnosis complete; native Wasmer trap-handler behavior remains unresolved. |
| **Severity** | Low | Fatal self-signals differ from Node; ordinary CLI and realm compatibility checks work. |

## Reproduction

`build-edge-quickjs-cli/edge tests/js/child-exit-signal.js` fails its first
`spawnSync` assertion: a child sending itself SIGSEGV can report status 0 and
signal null. The same public fixture passes on the V8 shipping CLI.

An isolated QuickJS child can print both before and after
`process.kill(process.pid, 'SIGSEGV')`; SIGBUS and SIGILL can behave similarly.
V8 and Node terminate with the requested signal. A Python subprocess probe
also observed QuickJS signal substitution (SIGSEGV/SIGBUS becoming SIGILL), so
the problem precedes Node child-process exit-status mapping.

## Diagnosis and follow-up plan

LLDB with SIGSEGV delivery enabled stops in
`wasmer_vm::trap::traphandlers::trap_handler`, reached through `_sigtramp`,
`uv_kill` and `ProcessMethodsKillCallback`. Native QuickJS bootstrap installs
its WebAssembly support through `EdgeInstallQuickJsWebAssembly`, which
initializes Wasmer's native host trap handlers. That integration is already
present in the starting Edge HEAD; the realm migration does not add it. This
is source evidence of an existing integration hazard, not a claim of an old
binary baseline rerun.

A standalone C-API reproducer using only the installed Wasmer 7.5.0 headers
and `libwasmer.a` (`wasm_engine_new`, `wasm_store_new`, `raise(SIGSEGV)`) also
terminates with SIGILL instead of SIGSEGV, independently of EdgeJS and N-API.
Artifacts and LLDB evidence are under
`/private/tmp/edge-realms-child-signal-wasmer*`.

The follow-up belongs in the Wasmer trap/signal integration: distinguish
user-generated signals from hardware traps and chain or restore the previous
handler when the signal is not a Wasm fault. Reproduce the standalone native
QuickJS child and inspect the host handler before changing signal semantics.
Preserve WebAssembly trap handling and keep this fixture enabled; do not mask
the failure by weakening its assertions or changing process.kill.

Tracked during [the N-API upgrade](../../../dev_012_napi_realms_upgrade/001_runtime_integration.md).
