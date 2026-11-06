# N-API realms upgrade: independent integration review

| Field | Value |
| --- | --- |
| Status | Complete |
| Scope | Read-only review of realm/bytecode/module lifetime and foreground hook integration. |
| Dependencies | Current working tree with N-API main plus targeted QuickJS sandbox evaluation fix. |
| Write ownership | Review worker owns no code; report actionable issues to primary. |
| Verification | Inspect both backends' API contracts; identify ownership, exception, reentrancy and termination regressions. |

Other workers are running native and WASIX tests. Do not modify their files or
start expensive builds. Concentrate on correctness and scope rather than style.

The review confirmed realm and bytecode target consistency, scoped realm
lookup, absence of context-token consumers, and foreground task handoff and
cleanup ownership. It found one real regression in the initial QuickJS module
sandbox fix: source-style writeback overwrote principal-realm synthetic
callbacks' direct sandbox mutations. The module worker added regressions and
limited the synchronization boundary to source execution, with explicit
source/host boundary synchronization around synthetic callbacks. Follow-up
coverage includes nested callbacks and the import-meta callback.

The reviewer subsequently implemented the separately scoped raw-test
subprocess fix; results and its remaining IPC caveat live in
`troubleshooting/node-compat/edgejs/015_v8_raw_test_fork_safety.md`.
