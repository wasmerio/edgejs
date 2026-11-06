# N-API realms upgrade: runtime integration

| Field | Value |
| --- | --- |
| Status | Complete; remaining engine/runtime compatibility gaps are documented below. |
| Scope | Upgrade EdgeJS to current N-API main; migrate deprecated contextify and context-token integration to explicit realms. |
| Dependencies | N-API main `18209d2`; preserve all pre-existing local changes. |
| Write ownership | Primary worker: `napi` gitlink, `src/`, development index and this note. |
| Verification | Native V8 and QuickJS builds, focused vm/module/bytecode checks, Edge-owned CTest and Node compatibility suites; WASIX build and smoke where available. |

The starting N-API checkout was clean at `a65e9cb`, already ahead of Edge's
recorded gitlink `1852baf`. Existing local edits include the architecture review
in `troubleshooting/node-compat/napi/003_contextify.md`, the QuickJS package
version, and untracked tests/artifacts. Preserve them.

Read `napi/docs/realms.md` and the realm contracts before changing runtime
code. Realm handles are scoped: retain globals/sandboxes through `napi_ref`
and recover handles with `unofficial_napi_get_realm` at each use. Native Edge
callbacks belong to the principal realm, matching Node. Avoid entering a vm
realm around calls to principal-realm native bindings. Use explicit realms for
scripts, compiled functions, modules and bytecode.

Plan: audit call sites and lifetime ownership, update integration, reproduce
and diagnose individual compatibility failures before narrow fixes, run the
relevant runtime suites, then record exact results and limitations here.

## Final implementation

- Advanced the N-API checkout/gitlink to current main
  `18209d2c80dd1404644fbbd88eff4771993e142f` (PR 127's realm model), including
  QuickJS `870aa7b604792cf38c597e9f8d57c19b3c05afea`.
- Replaced deprecated contextify script/function/context creation and module
  context fields with explicit realms. `edge_realm.h` resolves each sandbox
  at use time. No scoped realm handles are stored in Edge state, and native
  bindings keep principal-realm identity.
- Removed unused context-token hooks and Environment token tracking.
- Passed target realms to cache compilation as well as function/module
  consumption; a cache never accidentally selects the principal realm.
- Adapted empty linking of evaluated synthetic builtin facades without
  weakening the provider's module-state validation (issue 014).
- Updated foreground task handoff, exact cleanup, delayed scheduling and
  shutdown refusal to the new provider contract (subtask 003).
- Replaced the raw test harness's post-fork engine initialization with
  `posix_spawn`/fresh exec, preserving captures, TTY handling and sharding
  (issue 015).

The Edge gitlink includes two published dependency fixes, atop N-API main
`18209d2`, in `4179e538007b12b4b28cd8f1bc521990e09e0a3b` on branch
`codex/edgejs-realms-integration`: QuickJS module sandbox synchronization with
explicit synthetic/import-meta host callback boundaries, and guarded
imported-cache Buffer callbacks (issue 023). Their provider-owned C++/Rust
regressions also live inside N-API. The companion N-API PR must land before
the dependency is folded back into main.

Companion: [wasmerio/napi#140](https://github.com/wasmerio/napi/pull/140).

The final dependency commit also corrects a provider-owned regression test:
V8 reports a standalone synthetic evaluation callback's exception directly,
whereas source-root evaluation yields a rejected promise. Reproduced the
exact CI failure with the pinned V8 11.9.9 build and inspected it in LLDB
before adapting that test expectation. Runtime behavior is unchanged.

The HTTP benchmark follow-up upgrades the standalone Wasmer dependency to
current main `4434ce0` plus the nonblocking async-store API fix `a021148`.
Rust and concurrency CI use 1.99; optional `cli-llvm` selects LLVM 23 with
compiler-specific caches. Host-JS adapters use public store handles, explicit
parking and current memory-buffer accessors. Issue 024 records the additional
callback ownership regression and upstream fix; subtask 005 records exact
benchmark versions, settings and results.

## Verification

| Check | Result |
| --- | --- |
| Native V8 `make build JOBS=4` | Pass |
| Native QuickJS `make build-edge-quickjs-cli JOBS=4` | Pass |
| V8 `make test-only TEST_JOBS=4` | 1,749 passed, zero failures |
| QuickJS `make test-quickjs-only TEST_JOBS=4` | 1,741 passed, zero failures; existing configured worker/TLS skips retained |
| Public realm fixture | Pass on Node 26.0.0, native V8, native QuickJS, embedded QuickJS WASIX and imported-N-API V8 WASIX |
| Bytecode-cache end-to-end suites | Both native providers pass |
| N-API-owned module-state runner | V8 30/30 and QuickJS 31/31 pass |
| N-API-owned imported cache callback regressions | 3/3 pass |
| Foreground task contract | Three new regressions and existing nested-immediate case pass |
| Final V8 Edge-owned CTest | 1,431 passed, 18 existing skips, one intermittent IPC failure out of 1,450 |
| Final QuickJS Edge-owned CTest | 6/7 pass; the fatal-signal test exposes installed Wasmer behavior (issue 016) |
| Embedded QuickJS WASIX rebuild and import gates | Pass; no N-API or unexpected non-WASI imports |
| Imported WASIX rebuild and validator | Pass; 110 standard N-API and 68 extension imports; engine-free |
| Rebuilt standalone N-API Wasmer runner | Pass with locked release build |
| Both Wasmer package checks | Pass |
| Embedded WASIX deterministic safe-mode smoke | 7/7 pass |
| Import validator regression suite / wasm32 ABI probe | 8/8 pass / compile passes |
| Root and submodule `git diff --check` | Pass |

Full Node compatibility sweeps preceded the last narrow module callback and
raw-harness refinements; the final module/provider fixtures and full Edge-owned
CTest were rerun after those refinements. N-API implementation tests were run
from N-API itself, with no duplicate root Makefile or CI jobs added. In this
existing build tree, stale provider CTest manifests remain from older configs;
the root CTest verification explicitly uses `-R '^edge\.'`.

Final logs are under `/private/tmp/edge-realms-*`: `v8-spawn-ctest.log`,
`quickjs-complete-ctest.log`, `*-final-tests.log`, `*-bytecode.log`,
`wasix-complete-{build,realms,smoke-warm}.log`,
`imported-wasix-complete-{build,realms}.log`, and
`napi-wasmer-final-build.log`.

## Remaining limits

- QuickJS sandbox globals still use copy-in/copy-out emulation. Properties
  written after top-level await are not immediately reflected in the original
  sandbox. See the canonical contextify note and subtask 002.
- Native QuickJS fatal self-signals are intercepted by the installed Wasmer
  7.5.0 host trap handler. A standalone Wasmer C-API reproducer establishes the
  same problem independently of Edge/N-API (issue 016).
- The child-process IPC/nextTick raw test intermittently delivers fewer than
  ten uncaughtException callbacks. This reproduces before the harness change,
  after it, and in isolation; it remains enabled (issue 015).
- Outbound network/framework app suites were not run. WASIX validation covers
  package/import contracts, realm behavior and deterministic lifecycle smoke.

All pre-existing local changes, package versions, untracked fixtures and test
submodule artifacts were preserved.
