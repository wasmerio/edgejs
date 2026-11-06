# Lazy property realm lifetime and native integration

| | | Remarks |
| --- | --- | --- |
| **Status** | 🟢 | Core property, realm and native API fixes verified; serialization diagnostics deferred per the requested scope. |
| **Severity** | Medium | Lazy definitions retain an unrelated realm, and one termination status is incorrect on current main. |

## Current scope

The user requested a substantially smaller QuickJS PR. Serialization, retained
SAB tables, worker changes and clone/message exception-transfer changes below
were investigated and verified, then removed from this PR as separate work.
Their diagnostic history is retained for follow-up; they are not part of the
final implementation. The final gate covers lazy property semantics, realm/GC
lifetime, filtered enumeration and native writes.

## Reproduction and action plan

N-API main `49ae989` plus PR 137 (`a6bbe8a`) and the shared test port:
`napi_v8_test_75_unofficial_lazy_data_properties` passes 20/22. The new
`DefinitionDoesNotRetainTheCallingRealm` test finds that the V8 definition array
retains the temporary current realm even when holder/initializer are from the
principal realm. The existing `V8CallsKeepTermination` test reports `napi_ok`
from the lazy query after execution termination, instead of
`napi_pending_exception`.

Reproduce each exact filter and inspect in LLDB before changing it. First make
the internal V8 definition array in the principal realm, then rerun the lifetime
test. Separately inspect current-main termination bookkeeping and adapt the two
lazy calls to the existing provider termination helper. Avoid changing global
termination semantics. Rerun the complete lazy runner and relevant realm suite.

Logs: `/private/tmp/edgejs-lazy-v8-test-baseline.log`,
`/private/tmp/edgejs-lazy-v8-realm-lldb.log`. Integration is tracked in
[the implementation note](../../../dev_013_quickjs_lazy_properties/001_implementation.md).

## QuickJS integration checks

The first native QuickJS run passes 17/19 tests. `Object.freeze` currently
initializes an untouched lazy property through `JS_DefineProperty`; V8's freeze
keeps it lazy. The exact failing filter was inspected in LLDB before editing
(`/private/tmp/edgejs-lazy-qjs-freeze-lldb.log`). Adjust the integrity-level
attribute update path while retaining descriptor-read semantics for explicit
`Object.defineProperty`, and rerun the targeted test first.

The other failure is an existing JavaScript typed-array sanity assertion, not
the new N-API validation: all canonical numeric names are rejected correctly by
the lazy API, while QuickJS's existing `Reflect.defineProperty` accepts at least
one of those names. Reproduce against the baseline engine before deciding
whether to retain that V8-specific assertion only for V8.

Baseline native Edge QuickJS confirms only `NaN` is accepted by that raw
`Reflect.defineProperty` sanity check. The existing numeric-index helper has an
explicit TODO for this case. Keep that original V8 sanity assertion V8-only;
all eight canonical-name rejection checks remain shared and unchanged. No broad
typed-array semantic change is part of this feature.

Read-only review also found the engine serializer rejects all non-normal
property tags. Add targeted lazy-value serialization coverage, then snapshot
keys and retrieve lazy values safely if needed; never keep shape/property
pointers across an initializer or recursive serialization call.

Provider review identified two additional integration paths to test before
changing them: filtered name enumeration currently requests full descriptors
(initializing lazy values), and the inherited readonly assignment compatibility
fallback also reads full descriptors. Add shared native API tests and inspect
the exact failures. Expose the engine's existing flags-only property lookup for
attribute filters. For assignment, compare V8 first and preserve the existing
provider compatibility bridge's behavior outside user-lazy properties.

The extended shared runner passes 29/29 on V8. QuickJS reproduces five new
failures: native inherited writes initialize and shadow lazy properties;
filtered enumeration runs the throwing initializer; native-function mutation
errors use the reader realm; clone and serializer reject the lazy property tag.
LLDB reproductions are saved as `/private/tmp/edgejs-lazy-qjs-<test-name>-lldb.log`.
The old eager inherited-write fallback deliberately shadows on QuickJS, so its
existing own-key result is not a shared lazy-property invariant. Scope the new
test to lazy writes and keep eager status comparison only. Preserve that older
compatibility fallback for ordinary properties.

For native-function error realms, attach creation-realm metadata to QuickJS
C-function-data records and trace/free it with the function. This lets lazy
initialization find the real realm without changing general C-function-data
invocation semantics or adding a provider-only side table.

After the engine serializer supports lazy reads, an initializer can throw from
the provider's clone/serializer paths. Their existing write-failure branches
leave the raw QuickJS exception untransferred to the N-API env, returning generic
failure or a null callback result. Add clone and serializer throw/retry tests,
reproduce under LLDB, then capture those exceptions with the existing
`return_pending_if_caught` helper. Keep resource cleanup and non-exception failure
behavior intact. This is the last planned provider integration adjustment.

Review found a concrete shared-buffer lifetime hazard in callback-capable
serialization: `{buffer: sab, value: lazy}` can serialize `sab`, then the
initializer deletes `buffer` and collects it. Writer traversal roots end before
the raw backing pointer is consumed by clone/message code. Add an opt-in
`JS_WRITE_OBJ_SAB_RETAIN` mode with an explicit table-release API, use it in
clone/message paths, and cover removal/GC under sanitizers. Retain the existing
borrowed-table API for callers which do not opt in. The default V8 Serializer
rejects shared buffers unless an ID hook is supplied; QuickJS does not implement
that hook. Match that default rejection in its raw Serializer rather than
emit an unowned shared-buffer pointer into a durable byte buffer.

Final shared exception tests also expose the same missing exception transfer
in the V8 public clone wrapper: it returns `napi_pending_exception` but leaves
the initializer error uncaught rather than saving it in the env. The exact
test was inspected in LLDB (`/private/tmp/edgejs-lazy-v8-clone-exception-lldb.log`).
Add a local TryCatch at the public API boundary and preserve termination using
the existing provider helper, then rerun the shared exception and full V8 suites.
The same regression now covers `message_create` and its identical missing
exception-transfer boundary, keeping the fix local to both public calls.
