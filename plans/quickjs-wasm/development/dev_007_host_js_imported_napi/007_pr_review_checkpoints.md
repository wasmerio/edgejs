# N-API Surface Reduction PR Review Checkpoints

| | | Remarks |
| --- | --- | --- |
| **Status** | ✅ | Checkpoints A and B are implemented and verified locally on the Edge.js and N-API `codex/napi-surface-reduction` branches. |
| **PRs** | [napi#59](https://github.com/wasmerio/napi/pull/59), [edgejs#147](https://github.com/wasmerio/edgejs/pull/147) | Review feedback is handled by ownership boundary, not comment order. |
| **Invariant** | Provider-neutral Edge logic | The imported provider owns process-wide V8 state; JavaScript compatibility layers fail explicitly where metered host services are unavailable. |

## Checkpoint A implementation status

### Shared-host flag ownership (2026-09-28)

When Edge.js imports N-API from a shared Wasmer host, its startup request must
carry an empty `engine_flags` string. The host has already configured V8 for
all workloads in that process, and changing process-wide flags from one guest
would affect other instances. Embedded native providers retain the existing
`BuildSupportedV8Flags` behavior. A rebuilt WASIX Edge.js guest with the
empty request loaded a JavaScript module through the Edge workload manager and
printed `EDGEJS_LOADER_OK=42`; the instance exited and its live registry
drained. The previous guest sent default V8 flags and failed during runtime
configuration before module loading.

The shared host also denies persistent bytecode handles and native V8
serialization until their retained buffers can be charged to the workload.
For imported N-API, `vm.Script` now validates syntax through a transient,
compile-only provider call; construction does not execute the script.
`cachedDataRejected` and `cachedDataProduced` report the unavailable cache
honestly, and `createCachedData()` raises an explicit unsupported error.
`vm.compileFunction` compiles directly from source under the same cache policy.
`node:v8` retains its metadata APIs while Serializer and Deserializer fail
explicitly on use. The test-runner's V8 result header is initialized only when
parsing child-process results, so importing `node:test` does not try to create
an unsupported serializer. The child-process serialization path remains
unavailable on this provider.

The focused `tests/js/host-imported-napi-compat.js` probe passed through the
real Edge workload manager against N-API `3231ebc` and Wasmer `dd7c498` with
`EDGEJS_HOST_NAPI_COMPAT_OK`. It covers imports, explicit serializer denial,
syntax errors, constructor no-execution, source-backed execution, and cache
status. The same rebuilt guest also passed the ESM import loader smoke with
`EDGEJS_LOADER_OK=42`; both exited successfully and drained Edge's live
instance registry.

The final clean WASIX build used the committed Edge.js branch and its N-API
submodule at `c13f5d3`, producing guest SHA-256
`901a369bedcfe3e6b0070ef0be2c9da1c022879f98dddc038d784bc2f8f3ccb6`.
The import validator found 110 standard N-API and 68 extension imports, with
no embedded engine. Against Edge's published Wasmer integration revision, the
host compatibility probe and two-module ESM loader probe both passed through
the workload manager, exited with code zero, and drained the live registry.

| Boundary | Status | Implementation |
| --- | --- | --- |
| A1: wire descriptors | ✅ | Native descriptors are separated from fixed-width wasm32 layouts, with compile-time C/C++ and Rust size checks and named Rust offsets. |
| A2: ownership and hooks | ✅ | Guest heaps are rejected or transferred explicitly; providers return an accepted hook mask; Edge-owned cleanup and destroy callbacks were removed from the provider contract. |
| A3: runtime lifetime | ✅ | A versioned, explicit process-runtime configuration call owns engine flags. Environment creation contains only per-environment state, and workers cannot configure the runtime. |
| A4: compatibility gate | ✅ | The validator compares all Edge standard and extension imports with both Wasmer N-API provider implementations, CI records the revision triplet, and the rebuilt SDK-branch Wasmer instantiates the resulting Edge artifact. |

The runtime is intentionally modeled as one explicitly configured process
singleton rather than as multiple opaque handles. That matches V8's actual
lifetime, prevents the first environment from choosing process flags, and
avoids pretending that independently releasable runtimes exist where they do
not.

## Decision

The reduction is directionally correct, but exported function count is not the
primary success metric. The boundary is successful only when each operation
has one semantic owner, fixed lifetime rules, and a representation that cannot
silently change between native and wasm32 builds.

The review exposed four boundaries which must be made explicit before further
surface reduction:

1. native C ABI versus the wasm32 wire representation;
2. process-global runtime configuration versus per-environment state;
3. required embedder callbacks versus optional or unsupported callbacks; and
4. the compatible Edge/N-API/Wasmer protocol version triplet.

## Checkpoint A: protocol, ownership, and runtime lifetime

Checkpoint A is complete only when the following are true.

### A1. Fixed-width wire descriptors

- Native public descriptors retain natural C types where they represent native
  addresses or sizes.
- Every descriptor decoded from guest linear memory has a fixed-width wasm32
  wire definition. Wire pointers, handles, sizes, and lengths are `uint32_t`;
  counters which are genuinely 64-bit remain `uint64_t`.
- Rust decoders do not contain unexplained numeric offsets. Each offset is tied
  to a named wire layout and checked by compile-time C/C++ and Rust layout
  assertions.
- Descriptor `size` and `version` are validated before reading or taking
  ownership of any later field.
- The bytecode version begins at version 1 unless an older version was actually
  shipped and remains supported.

The purpose is not to introduce a generic opcode protocol or bindgen dependency.
Provider implementations remain typed and handwritten. The wire layer exists
only where a wasm32 pointer crosses into Wasmer.

### A2. Resource ownership and hook capability

- `guest_heap` has one documented owner on every success and failure path.
- A bridge which cannot accept a transferred resource rejects it before
  ownership transfer; it never ignores the pointer.
- `attach_env` must not report success for required callbacks it discards.
- Callbacks controlled by Edge's own lifecycle should be removed from the
  provider contract instead of being advertised as provider capabilities.
- Remaining provider-driven hooks are attached in one transaction. The
  provider either accepts the required set or returns an explicit failure.

### A3. Process runtime versus environment lifetime

- V8 flags and other process-global initialization are not configured by an
  arbitrary first environment.
- Process/runtime configuration has a typed owner and is initialized once;
  environments are created from that initialized runtime.
- Identical repeated configuration is idempotent. Conflicting configuration is
  rejected before any environment is partially created.
- Worker creation cannot change or accidentally become the source of the
  process-global configuration.
- QuickJS and host-JavaScript providers implement the same lifetime contract,
  even when their runtime owner is lightweight.

### A4. Protocol compatibility gate

- The Edge wasm import inventory is compared with the Wasmer provider exports
  before the slow compatibility suite starts.
- CI records the exact Edge, N-API, and Wasmer SDK revisions used together.
- A newly added operation must be implemented or explicitly rejected in V8,
  QuickJS, the native Rust bridge, and the host-JavaScript bridge before N-API
  CI can be green.

### Verification for Checkpoint A

1. N-API descriptor-validation and ownership tests for V8 and QuickJS.
2. A wasm32 layout/conformance test for every versioned wire descriptor.
3. Runtime tests covering identical configuration, conflicting configuration,
   main-environment-first, and worker/environment ordering.
4. Wasmer native and host-JavaScript bridge compilation.
5. Edge native V8 and QuickJS targeted environment/worker tests.
6. Edge WASIX import-conformance and instantiation smoke tests using the pinned
   Wasmer SDK branch.

Local evidence captured on 2026-08-14:

- V8 and QuickJS native provider builds pass.
- The focused V8 N-API suite passes 30/30 tests; the focused QuickJS N-API
  suite passes 28/28 tests.
- Edge V8 and QuickJS version/evaluation smokes pass, including worker teardown
  in both providers.
- All eight import-validator unit tests pass.
- The engine-free Edge WASIX artifact builds and validates 110 standard N-API
  imports plus 67 extension imports against both Wasmer provider inventories.
- The Wasmer N-API crate compiles against
  `/Users/syrusakbary/Development/wasmer` natively and for
  `wasm32-unknown-unknown` with the host-JavaScript feature.
- A clean worktree of the Wasmer SDK branch rebuilt against this N-API tree
  instantiates `build-wasix/edgejs.wasm` with `--experimental-napi`; `--version`
  reports `v24.13.2-pre`.
- `git diff --check`, Rust formatting checks, and the no-`edgejs/lib`-changes
  invariant pass.

## Checkpoint B: observable behavior

Checkpoint B fixes behavior changed while adopting the reduced protocol:

- QuickJS heap statistics and centralized `valid_fields` normalization;
- profiling start-result consistency;
- requested-field error metadata without unnecessary JS re-entry;
- unified sync/async `--abort-on-uncaught-exception` policy;
- iterative HTTP/2 input consumption;
- native tick-callback trampoline and transactional reference replacement;
- exact call-site frame counts;
- worker-thread-affine profiler ownership;
- DOMException bootstrap fallback;
- metadata-header parsing without a fixed line limit; and
- honest WASIX total/free/available memory reporting.

Each fix requires a targeted regression test before the wider suites run.

### Checkpoint B implementation status

| Behavior | Status | Root-cause fix |
| --- | --- | --- |
| Heap statistics | ✅ | Providers declare measured fields and consumers normalize unsupported fields once at the N-API boundary. QuickJS reports its current allocator value as the only honest peak lower bound and accounts for binary-object memory consistently. |
| Profiling start | ✅ | A provider which does not support profiling fails without writing contradictory success outputs. Worker-owned profiler tokens now live exclusively on the worker engine thread. |
| Error metadata | ✅ | Callers request a side-effect-free thrown-at-only snapshot when source and position metadata are not needed; V8 no longer invokes source-map JavaScript for that query. |
| Fatal exception policy | ✅ | Sync, async, capture-callback, domain-handler, and domain-handler-failure paths share one abort decision. A domain may handle the original exception, but cannot recapture an exception thrown by its own handler. |
| HTTP/2 input | ✅ | Parser pause/resume consumption is iterative and preserves reentrant input ownership without recursive tail calls. |
| Task queue | ✅ | Edge retains and invokes the original tick callback and receiver directly. All native-to-JS entries share one callback-scope nesting counter, so only the outer boundary checkpoints. The provider no longer rewrites formatted stacks, and no JavaScript wrapper is compiled at runtime. |
| Call sites | ✅ | Edge asks the provider for exactly the requested bounded frame count, including the 200-frame limit. |
| DOMException bootstrap | ✅ | A configurable late-binding accessor survives early bootstrap ordering and replaces itself with the final data property after resolution. |
| Test flags | ✅ | The Node-test runner parses the complete comment/directive metadata prefix and stops at the first executable statement; it has no fixed line limit. |
| WASIX memory | ✅ | Total/constrained memory remains the runtime ceiling while free/available memory subtracts committed wasm linear memory. |

### Verification for Checkpoint B

Local evidence captured on 2026-08-14 and 2026-08-15:

- The complete N-API suites pass for V8 (92/92) and QuickJS (88/88).
- Edge runtime and internal-binding suites pass (15/15 and 5/5), including
  native tick dispatch, exact 200-frame capture, and late DOMException bootstrap.
- Focused upstream Node regressions pass for HTTP/2 backpressure and domain
  abort behavior; metadata-prefix parser regressions also pass.
- Direct process probes abort synchronous and next-tick exceptions with status
  134, while an installed uncaught-exception capture callback prevents abort.
- The engine-free WASIX artifact validates 110 standard N-API imports and 67
  extension imports, and its build leaves `edgejs/lib` unchanged.
- A clean worktree of Wasmer SDK revision `a79deeff0b3`, with N-API revision
  `4462d18`, builds and instantiates the artifact with `--experimental-napi`.
  Version, exact-call-site-count, and total/free-memory smokes pass.
- The same Wasmer/N-API combination compiles for `wasm32-unknown-unknown` with
  the host-JavaScript and WASIX features.
- The full native Edge suites pass for V8 (1749/1749) and QuickJS
  (1741/1741). The complete WASIX V8 lane passes 1673 tests; its three
  load-sensitive failures each pass in isolation.
- The unmodified wasmer-sh browser smoke passes DNS, curl, nested `node`,
  `execSync`, symlink shebangs, V8 heap statistics, fork IPC, and a real pnpm
  install. The pnpm worker exposed a provider-lifecycle defect: runtime flags
  were process-global by contract but thread-local in the host-JS provider.
  Synchronizing that configuration across guest pthreads allows worker N-API
  environments to be created without any pnpm-specific workaround.
- `git diff --check` passes in both repositories.

### Task-queue ownership model

The task queue is deliberately split across two owners:

1. Edge owns Node semantics: callback-scope nesting, `tickInfo`, the retained
   `processTicksAndRejections` callback, and the decision to drain it.
2. The N-API provider owns engine and host progress through the single
   `unofficial_napi_event_loop_checkpoint` operation.

There is no provider tick operation, provider-kind branch, dynamic lookup of
`process._tickCallback`, runtime-compiled trampoline, or provider-side stack
filter. `napi_make_callback`, explicit callback scopes, Edge event callbacks,
top-level execution, and tick dispatch all observe the same nesting depth.
That makes the rule local and testable: closing the outermost successful
native-to-JS boundary performs at most one task-queue checkpoint.

A handled exception also ends that callback boundary. Acceptance by
`process._fatalException` says that the process may continue; it is not a
progress signal and does not authorize Edge to re-enter the provider checkpoint
in a native retry loop. Any remaining task-queue state is observed by the next
event-loop turn. `EDGE_TRACE_CALLBACKS=1` traces callback depth, retained-tick
dispatch, microtask checkpoints, and provider checkpoints when this ownership
needs to be diagnosed without changing scheduling.

This distinction was verified by replaying the callback-scope changes one at a
time from the last working package. Callback-depth unification, entry/timer
guards, retained task-queue state, and public `napi_make_callback` checkpointing
all preserved progress. Only immediate checkpoint retry after a handled worker
exception caused Next.js trace collection to monopolize the browser thread.
With one checkpoint per boundary, the native regression suite and WASIX async
filesystem stress pass, and a Next.js 16.3 production build completes browser
trace collection in 9.3 seconds before `next start` serves the result.

## Checkpoint C: packaging and scope separation

- Keep the intentional package identity `syrusakbary/edgejs` and the `edge`,
  `edgejs`, and `node` commands, and document that decision in the PR.
- Move npm out of the Node test submodule into a pinned production asset.
- Keep release-only version bumps separate from protocol commits.
- Move Next.js/WebContainer compatibility commits into their own follow-up PR.
- Publish a removed-operation mapping, provider capability matrix, ownership
  table, exact revision triplet, and test evidence in the PR descriptions.

## Non-goals

- No changes under `edgejs/lib`.
- No provider selection in Edge runtime logic.
- No generic `unofficial_napi_call(opcode, ...)` dispatcher.
- No compatibility stub which claims to implement an unsupported capability.
- No unrelated Next.js, WebContainer, HTTP, parser, or packaging work during
  Checkpoint A.

## Merge order

1. Land Checkpoint A in N-API and update Edge to its exact revision.
2. Pin the compatible Wasmer SDK revision and pass the import gate.
3. Land Checkpoint B as focused correctness commits.
4. Move Checkpoint C follow-ups out of the protocol PR where practical.
5. Merge N-API first, then Edge.js; publication follows the merged revisions.
