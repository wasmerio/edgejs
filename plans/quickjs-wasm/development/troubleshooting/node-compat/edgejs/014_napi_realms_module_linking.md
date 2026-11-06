# N-API realms upgrade: linking evaluated synthetic facades

| | | Remarks |
| --- | --- | --- |
| **Status** | 🟢 | Edge's adapter accepts the loader's empty link on evaluated synthetic facades; both native engines pass public import regressions. |
| **Severity** | High | Ordinary ESM imports of Node builtins fail with `ERR_VM_MODULE_ALREADY_LINKED`. |

## Reproduction and diagnosis

`build-edge-quickjs-cli/edge test/fixtures/warning_node_modules/new-buffer-esm.mjs`
fails at `ModuleJobBase.syncLink` -> native `ModuleWrapLink`. LLDB breakpoint
`unofficial_napi_module_wrap_link` confirms this call path. The V8 Node
compatibility lane reports the same error.

`BuiltinModule.getESMFacade()` in `lib/internal/bootstrap/realm.js` deliberately
instantiates and evaluates a dependency-free synthetic module immediately.
The normal loader subsequently calls `link([])` on this facade. The new
provider rejects linking modules once instantiated, while Node's internal
ModuleWrap link merely fills the resolution cache and tolerates this empty call.

## Action plan

Keep the provider's strict state validation. In Edge's ModuleWrap adapter only,
treat `link([])` on an already instantiated dependency-free synthetic module as
a no-op. Continue validating the argument array and continue forwarding source
modules, nonempty links and uninstantiated synthetic modules to the provider.
Add public builtin-import coverage and rerun the exact failing fixture, relevant
module checks, then both Node compatibility lanes.

## Result

`ModuleWrapInstance` records whether it represents a synthetic module. Only
`link([])` on a synthetic module whose provider status is at least instantiated
returns without calling the provider. The public fixture
`tests/js/napi-realms.js` covers `import('node:buffer')` and named/default imports
of a generated CommonJS module on Node, V8 Edge and QuickJS Edge. All pass.
The provider continues to reject invalid linking transitions.

The reference is [Node's internal ModuleWrap link implementation](https://github.com/nodejs/node/blob/v24.0.0/src/module_wrap.cc#L465),
which fills the resolution cache without relinking an evaluated module.

This issue belongs to
[the realm upgrade](../../../dev_012_napi_realms_upgrade/001_runtime_integration.md).
