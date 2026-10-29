# Known Issue: Host-JS module compiler drops import attributes

| | | Remarks |
| --- | --- | --- |
| **Status** | ▶️ | Root cause confirmed; fix planned in the N-API host-JS provider. |
| **Severity** | High | Any package whose ESM graph uses `with { type: "json" }` fails to load on the deployed host-JS runtime. |

Tracked by [wasmerio/edgejs#175](https://github.com/wasmerio/edgejs/issues/175).

## Symptom

Importing `@earendil-works/pi-ai` on the deployed Wasmer runtime (host-JS
backend, reports itself as Node.js v24.13.2) throws:

```text
TypeError [ERR_IMPORT_ATTRIBUTE_MISSING]: Module ".../pi-ai/dist/providers/data/amazon-bedrock.json"
needs an import attribute of "type: json"
```

The stack runs through `validateAttributes` → `defaultLoadSync` →
`ModuleLoader.load` → `ModuleJob.syncLink`, i.e. the synchronous ESM path used
by `require(esm)` and sync linking. The package's dist source carries the
attribute (`import values from "./data/amazon-bedrock.json" with
{ type: "json" };` in `dist/providers/amazon-bedrock.models.js`), and the same
script works on real Node 24.

## Reproduction

- Minimal repro (`import data from './x.json' with { type: 'json' }`, plus a
  `require(esm)` variant whose dependency carries the attribute) passes on the
  native Edge QuickJS CLI (`build-edge-quickjs-cli/edge`), on the Edge V8 CLI
  (`build-edge/edge`), and on the WASIX QuickJS build under
  `wasmer run build-quickjs-wasix/edgejs.wasm`. `npm i @earendil-works/pi-ai`
  plus `import * as pi from '@earendil-works/pi-ai'` also loads on both local
  backends. The failure is specific to the host-JS (wasm-bindgen "snapi")
  backend used by the deployed runtime.
- The host-JS backend cannot run locally, but its module compiler is plain
  JavaScript embedded as a `wasm_bindgen(inline_js = ...)` snippet in
  `napi/src/snapi_js.rs`. Extracting the snippet and driving
  `wasmer_napi_compile_module` under Node with acorn 8.19 reproduces the bug:
  `import data from "./x.json" with { type: "json" }` (and the
  `export ... from` re-export forms) produce a module request whose
  `attributes` object is empty.

## Root Cause

`wasmer_napi_compile_module` in `napi/src/snapi_js.rs` parses module sources
with Acorn (`ecmaVersion: 'latest'`, which parses `with { ... }` clauses into
`node.attributes`) and lowers import/export declarations into module requests
with `requestIndex`:

```js
requests.push({ specifier, phase: 2, attributes: Object.create(null) });
```

The parsed `node.attributes` (ESTree `ImportAttribute` nodes on
`ImportDeclaration`, `ExportNamedDeclaration`, and `ExportAllDeclaration`) are
never copied into the request, so Node's ESM loader sees empty
`importAttributes`. The loader resolves `.json` as format `json`, requires
`type: json` in `validateAttributes`, and throws
`ERR_IMPORT_ATTRIBUTE_MISSING`.

The QuickJS provider is unaffected (the vendored QuickJS parses attributes and
`napi_module_wrap__::cache_module_requests` clones them into each request), and
the V8 provider reads V8's import-attributes `FixedArray` directly. The
dynamic-import lowering in the host-JS snippet already forwards the parsed
`node.options` source text, so only static module requests lose attributes.

## Action Plan

1. In `napi/src/snapi_js.rs`, extract the `with { ... }` attributes from the
   declaration node inside `requestIndex` (keys may be identifiers or string
   literals; values are string literals) and attach them to the pushed request,
   keeping an attribute-less declaration's `attributes` an empty
   null-prototype object.
2. Verify with the extracted-snippet Node harness: static import, named
   re-export, and star re-export each surface `attributes.type === 'json'`,
   and an attribute-less import keeps empty attributes.
3. Add shared N-API regression coverage in
   `napi/tests/programs/run_script_test.c`: compile a module importing
   `./data.json` `with { type: 'json' }` and assert the created module's
   request exposes `specifier` and `attributes.type === 'json'`. This runs on
   native QuickJS and V8 providers and in the host-JS `run-script-test`
   harness.
4. Run the napi native QuickJS and V8 suites; commit on a napi branch and open
   a PR against wasmerio/napi referencing wasmerio/edgejs#175.
5. In EdgeJS, add a runtime regression test covering JSON import attributes
   (async import and `require(esm)`) so both local backends stay covered, run
   `make test-quickjs-only`, and open the EdgeJS PR with this note, the
   registry update, and the napi submodule bump once the napi fix lands.

## Resolution

Fixed in the N-API repository by
[wasmerio/napi#126](https://github.com/wasmerio/napi/pull/126)
(`fix/host-js-import-attributes`, commit `c4316d9c`): `requestIndex` now copies
the declaration's parsed `with { ... }` attributes onto the pushed module
request, keeping an empty null-prototype object for attribute-less requests.

Verification:

- Extracted-snippet Node harness (acorn 8.19): static import, named re-export,
  and star re-export each report `attributes: { type: 'json' }`; an
  attribute-less import keeps `{}`. All four failed before the fix (empty
  attributes on the attributed forms).
- New shared gtest
  `Test65UnofficialContextify.ModuleRequestsPreserveImportAttributes` passes on
  native QuickJS (13/13 suite) and V8 (16/16 suite); the same assertions were
  added to `tests/programs/run_script_test.c` for the WASIX manifest and
  host-JS harnesses.
- New EdgeJS regression test `tests/js/esm-json-import-attributes.mjs`
  (ctest `edge.esm_json_import_attributes`) covers the static, dynamic, and
  `require(esm)` JSON-attribute paths; it passes on the Edge QuickJS CLI, the
  Edge V8 CLI, and real Node.

The deployed runtime picks up the fix once wasmerio/napi#126 merges and the
Wasmer web runtime rebuilds its `lib/napi` checkout. Status stays ▶️ until the
deployed runtime is confirmed fixed.
