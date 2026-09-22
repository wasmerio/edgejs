# QuickJS upstream master synchronization

Status: active. User requested all upstream master commits in the Wasmer QuickJS
fork, removal of superseded local patches, and N-API/EdgeJS PR CI validation before
default-branch updates. Feature branches and draft PRs are the integration gate;
no default branches will be pushed or merged during preparation.

Isolated checkout: `/private/tmp/quickjs-upstream-sync/edgejs`, with nested N-API
and QuickJS worktrees. Existing user checkout is left intact.

Baseline: EdgeJS `7027edfa`, N-API `8e1f413`, QuickJS `7b8ab77`.
Upstream master: `6d46d07` (v0.17.0); 129 upstream commits missing from fork.

| Subtask | Owner | Dependencies | Write ownership | Verification |
| --- | --- | --- | --- | --- |
| 001 fork audit | audit worker | fetched refs | audit note only | account for each fork delta, upstream replacements and APIs |
| 002 engine integration | coordinator | audit | QuickJS repo, summary/registry | engine unit/regression tests, upstream ancestry, sanitizer CI |
| 003 N-API adaptation | N-API worker | engine merge ready | N-API except engine gitlink; own note | Release/Debug N-API tests, CI |
| 004 EdgeJS validation | EdgeJS worker | engine and N-API ready | EdgeJS runtime if needed; own note | native/WASIX build, relevant runtime suites, CI |

Workers share the filesystem and must not modify other write sets. The coordinator
owns all commits, gitlinks, branch pushes, PR descriptions, and CI integration.

## Integration decisions

The [fork audit](001_fork_audit.md) accounts for 21 non-merge fork-master commits plus the consumed `ff1471c` stack
compatibility commit (22 downstream changes total). The engine merge uses upstream coroutine GC, WASI stack accounting,
private symbols, and module attribute identity. The old external ArrayBuffer
detach coordination and engine inspection API are deleted. Upstream fixes async
iterator close ordering; its handled-promise API replaces the N-API no-op.

Still required: host module APIs, native weak links, promise-reaction hooks,
WASIX atomics/thread guards, entry previews, stack inspection, lazy stack
formatting, and Node-specific presentation semantics. The fork therefore shares
all upstream history, but intentionally remains different from upstream's tree.

The consumed `ff1471c` Error-stack patch (not present on fork master) is retained,
including derived constructor filtering, receiver/type CallSite metadata, and
its correct non-exotic lazy accessor installation. New coverage verifies lazy
single formatting/capture/assignment with upstream Error storage.
Upstream bytecode changed to version 28: old cached bytecode must be regenerated
or rejected; no compatibility with old serialized engine bytecode is promised.

## PR dependency chain

1. QuickJS [#9](https://github.com/wasmerio/quickjs/pull/9), merges `ed3bfa5` and `0daca74`.
2. N-API [#75](https://github.com/wasmerio/napi/pull/75), commit `54d7989`.
3. EdgeJS [#154](https://github.com/wasmerio/edgejs/pull/154), pinned to that N-API commit.

Preserve the QuickJS merge ancestry; squash merging it would lose the upstream
history inclusion. Land dependencies in order only after all relevant CI is
green. PRs are drafts; default branches have not been updated.

## Verification so far

- QuickJS Debug: 117 bundled tests passed.
- QuickJS ASAN+UBSAN: 117 bundled tests passed.
- QuickJS ASAN+GC stress: 116 passed, existing stress exclusions retained.
- C/C++ headers, regexp tests, standalone hello compilation/execution passed.
- N-API QuickJS Release: 95/95; Debug: 95/95 (final engine code).
- Shared N-API tests on V8: 11/11.
- EdgeJS final native Release and WASIX builds succeeded; focused engine/stack
  checks, 77 console/diagnostics tests, safe-mode, Intl and package validation
  passed. The first full native run passed 1,741 tests; see the validation note
  for exactly which checks predated the consumed-branch merge.
- QuickJS and N-API CI are green on final heads `0daca74` and `54d7989`.
  The first EdgeJS run passed native QuickJS but hit one WASIX x509 stack trap
  plus two V8 failures (HTTP2 test and npm registry fetch). Final-head EdgeJS CI
  is the remaining gate; use the linked PR checks for current status.
  Baseline nightly publishing failure does not run on pull requests.

Known pre-existing caveat: standalone qjs static-module execution/`--std` can
crash because the fork expects host-driven module resolution. Reproduced in a
separate origin/master build under LLDB. Bundled tests resolve modules explicitly;
EdgeJS uses its host loader. See the audit note for reproducer and evidence.
