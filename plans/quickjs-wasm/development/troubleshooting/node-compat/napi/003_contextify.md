# Known Issue: Contextify diagnostics

| | | Remarks |
| --- | --- | --- |
| **Status** | 🟢 | Implemented as `napi_contextify__` under `napi/quickjs/src/internal`. |
| **Severity** | High | Contextify is central to script compilation, source metadata, and framework bootstraps. |

## Current State

Contextify helpers and state live in:

- `napi/quickjs/src/internal/napi_contextify.h`
- `napi/quickjs/src/internal/napi_contextify.cc`

`napi_env__` owns `napi_contextify__`. Source-map settings and callback state
are kept with that class instead of in removed compatibility files.

## Known Incompatibility

Node's contextify APIs sit under `vm`, internal loaders, and framework
bootstraps. QuickJS can compile and evaluate scripts, but its diagnostics and
context model do not naturally match V8 objects. Compile-time failures can be
annotated while QuickJS still exposes useful metadata. A caught `Error` returned
from JavaScript does not currently provide the same reliable V8-style message
object for preserved source-map output.

## Current Status

A fuller design should keep contextify as a small QuickJS subsystem with
explicit compiled-script objects, cache-data policy, source-map state, and
structured diagnostics. The documentation and tests should clearly separate
supported QuickJS diagnostics from V8-only caught-error formatting.

## 2026-11-06: Adopted Realm API In EdgeJS

Edge now builds against N-API main `18209d2`, whose realm API distinguishes
realm handles from the environment and keeps environment hooks shared. Edge's
script/function/module calls resolve each sandbox to a scoped realm handle at
use time; native Edge functions remain in the principal realm. Deprecated
contextify entry points, module context fields and unused context-token tracking
have been removed from Edge's integration. Cached functions and modules compile
in their target realms too.

The public `tests/js/napi-realms.js` fixture passes on Node, native V8 Edge and
native QuickJS Edge. It covers separate intrinsics, sandbox state, nested
execution, native callback realm identity, cached compilation, retained realm
functions/jobs, source and synthetic modules, and builtin/CommonJS imports.

Testing found that QuickJS module evaluation did not copy the initial sandbox
properties into its realm global. The local N-API change in
`quickjs/src/internal/napi_module_wrap.cc` synchronizes the sandbox before and
after source evaluation. Synthetic and import-meta callbacks synchronize at
their explicit source/host boundary so a callback's direct sandbox changes
are preserved, including nested and rejecting callbacks. Provider-owned
test_77 regressions cover these cases and async/sync evaluation; 31 module-state
cases pass. The added regressions fail before the corresponding fixes.

QuickJS still uses copy-in/copy-out sandbox emulation. Writes after a top-level
`await` resume are not immediately copied back into the sandbox: for
`await Promise.resolve(); globalThis.written = 2`, the module global sees 2 but
the original sandbox retains its prior value. V8 and Node reflect 2 immediately.
Nested execution and compiled-function writes share the documented copy model's
limitations. No broad promise scheduling changes were made to hide them.

See [the integration task](../../../dev_012_napi_realms_upgrade/001_runtime_integration.md)
for build/suite results and the foreground task ownership adaptation.
