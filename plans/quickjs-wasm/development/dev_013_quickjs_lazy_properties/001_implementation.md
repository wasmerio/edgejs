# QuickJS lazy data properties

Status: complete. User approved implementing N-API PR 137 on QuickJS using an
engine extension based on existing autoinit property storage. Existing local
contextify documentation, package-version edits and untracked files are outside
this task and must be preserved.

Baseline: EdgeJS `3bb9d0e`; N-API current main `49ae989` (includes realms PR 140),
plus PR 137 `944b837` cherry-picked as `a6bbe8a`; QuickJS master `78817fd`.
Branches: EdgeJS `codex/quickjs-lazy-properties`, N-API
`codex/quickjs-lazy-data-properties`, QuickJS `codex/lazy-data-properties`.

## Scope and ownership

| Subtask | Owner | Dependencies | Write ownership | Verification |
| --- | --- | --- | --- | --- |
| Engine | engine worker | agreed public API | `napi/quickjs/deps/quickjs/` only | engine regression, sanitizers and GC/realm lifetime checks |
| N-API | provider worker | engine API contract | QuickJS N-API implementation, CMake source list, header documentation | shared native lazy-property tests |
| Tests | test worker | PR 137 baseline | `napi/tests/runners/test_75_unofficial_lazy_data_properties.cc`, `napi/tests/guest_lazy_properties.rs` | shared V8/QuickJS behavior; realm API migration |
| Integration | coordinator | all above | gitlinks, branches/commits/PRs, this note and index | native Edge, native provider suites, WASIX rebuild/smoke |

Workers share the filesystem. Do not edit another worker's files. The coordinator
owns commits and publishing. No default branch is pushed or merged.

## Action plan

1. Preserve PR 137's API and observable contract, replacing its QuickJS stubs.
2. Add engine define/query functions; store a GC-traced initializer. Ordinary
   gets and descriptor reads materialize in place; key enumeration and existence
   checks do not. Writes/deletes do not initialize.
3. Keep exceptions retryable, guard recursive initialization, re-find properties
   after JS calls and check definition identity before committing the result.
   Never retain raw property-array pointers across a callback.
4. Preserve initializer realm, holder receiver, attributes/order, proxy rejection,
   index validation, and no-callback own-property query semantics. Current autoinit
   has four packed IDs; extending it must not silently overflow that encoding.
5. Reuse provider-owned behavioral tests on both engines, add GC and mutation
   coverage, adapt the guest fixture to the current explicit realm API.
6. Run engine/provider checks, native Edge runtime checks, and rebuild using
   `cd quickjs-wasm && ./build.sh` before WASIX/package smoke checks. Record
   exact results and any pre-existing failures here.

## Existing analysis

QuickJS's `JS_AutoInitProperty` currently calls trusted built-in initializers
which must not mutate the object, clears autoinit state on exception, and also
initializes on assignment. Those behaviors cannot be reused unchanged for the
PR's arbitrary JavaScript initializer. Descriptor lookup already distinguishes
value reads from existence/flags-only queries, providing the required foundation.
JS getter emulation cannot expose the required data-property descriptors or
materialize non-configurable lazy properties correctly.

## Scope reduction requested by the user

The initial engine PR 12 (`31fcc2a`, 937 additions / 52 removals) expanded into
serialization, retained SharedArrayBuffer tables, worker changes and duplicated
engine regressions. The user requested a much smaller PR containing only what
is strictly required. Remove that entire serialization/SAB/worker expansion and
its provider adaptations/tests. Keep the property API contract, realm/GC safety,
flags-only enumeration, native assignment behavior and focused regressions.

Simplify per-property ownership by using a private null-prototype JS definition
object, whose existing GC/refcount provides lifetime and identity. Do not add
another custom refcounted record if normal engine ownership suffices. The shared
N-API runner remains the main conformance coverage; engine tests should focus on
its public define/query APIs and the lifetime risks at the engine boundary.

Earlier broad validation passed 1,741 Edge QuickJS Node tests, 344 provider
QuickJS tests, 416 V8 tests, native/WASIX builds and smoke, plus targeted
ASAN/UBSAN checks. These results predate the scope reduction. Revalidate the
trimmed implementation and record its exact results below before publishing.

## Reduced implementation and review

QuickJS [PR 12](https://github.com/wasmerio/quickjs/pull/12) was merged as
`fcf1604` on November 6, 2026. Its tree is identical to the tested head
`42b41e5`: 429 additions and 9 removals in four files. The original expansion
was 937 additions and 52 removals. `quickjs-libc.c`, all object writers,
serialization ownership and SAB APIs match the baseline exactly. Engine tests
add 114 focused lines; the shared N-API runner supplies conformance coverage.

The remaining engine changes provide define/query and flags-only lookup,
reuse autoinit storage with private GC-owned definition objects, preserve
initializer realms, and guard retry/re-entry/mutation and holder lifetime.
Assignments and integrity-level updates bypass initialization as required.
C-function-data records expose their creation realm to the existing internal
realm lookup; general native invocation behavior is unchanged.

N-API [PR 137](https://github.com/wasmerio/napi/pull/137) is updated in place
at `4d8aba2`, based on current main `49ae989`, and pins merged QuickJS
`fcf1604`. Its native wrappers, argument
validation, filtered enumeration and readonly lazy writes remain. Serializer,
clone/message and SAB work is removed, including the additional V8 exception
changes. The small V8 changes required by current-main realm/termination
integration remain. N-API PR 137 must land before folding its pin into Edge
main; the QuickJS dependency has landed.

## Reduced implementation verification

| Check | Result |
| --- | --- |
| QuickJS Debug/Werror engine suite | 120 passed |
| Focused engine lazy and execution-state modes | Pass |
| Focused engine ASAN, UBSAN and forced-GC regression | Pass |
| Public C/C++ headers, both value layouts | Pass |
| Full QuickJS provider suite | 338 passed |
| Full V8 provider suite | 410 passed |
| Shared lazy conformance | QuickJS 21, V8 27 passed |
| QuickJS lazy conformance under ASAN/UBSAN | 21 passed |
| Native Rust unit tests and Wasmer guest lazy tests | 65 and 7 passed |
| Native QuickJS Edge build | Pass |
| Full Edge QuickJS Node compatibility rerun | 1,741 passed |
| Embedded QuickJS WASIX rebuild/import gate | Pass |
| Focused engine API regression compiled for WASIX and run under Wasmer | Pass |
| WASIX public realm fixture and HTTP echo | Pass |
| QuickJS package check | Pass |
| QuickJS PR 12 CI on `42b41e5` | All eight checks passed |

Logs use `/private/tmp/edgejs-lazy-*-trimmed-*` and
`/private/tmp/edgejs-lazy-trimmed-*`; the engine report is
`/private/tmp/edgejs-lazy-qjs-engine-verification.md`. The native Rust/guest
rerun uses `/private/tmp/edgejs-lazy-trimmed-rust-tests.log`.

Baseline caveats are unchanged: the unspecialized upstream API-test stack-format
assertion and sanitizer-only `bug904.js` assertion reproduce on clean engine
master. No repository test exclusions were added. The raw QuickJS typed-array
`NaN` definition gap predates this work; lazy API validation rejects it correctly.
Serialization of unmaterialized lazy properties is deliberately outside this PR.

## Merged QuickJS pin

After the user merged QuickJS PR 12, fetched master and verified that
`git diff --exit-code 42b41e5 fcf1604` is empty. N-API commit `4d8aba2`
advances only the engine gitlink to the merge commit. The focused engine
lazy-property regression and all 21 shared QuickJS lazy tests pass again.
No implementation changed, so the broader source-tree verification above
still applies. Edge PR 179 follows the updated N-API feature-branch pin.
