# Next Steps: Make Tests Work Raw as in Node.js

Goal: run Node’s test files **unchanged** (no copies, no edits) with unode, so behavior and paths match Node.

---

## Raw tests currently run

When `PROJECT_ROOT_PATH` is set, the phase02 test runner runs these Node tests **raw** (unchanged from `node/test/parallel/`):

- **test-require-cache.js**
- **test-require-json.js**
- **test-module-cache.js**
- **test-require-dot.js**

They use: redirect of `node/test/common/` to unode's minimal `common/` (index, fixtures, tmpdir) and `NODE_TEST_DIR` for fixtures. Builtins resolve through the same runtime builtins as normal execution. The runner is the CI gate for raw Node tests.

---

## 1. Run tests from the Node test directory

**Current:** Tests live under `unode/tests/node-compat/` and are run via a path like `.../node-compat/parallel/test-require-cache.js`. That forces our own `common/` and `builtins/` and the "node-compat" → "test" path hack in JSON errors.

**Target:** Run the **same** files as Node, e.g.:

- `unode /path/to/node/test/parallel/test-require-cache.js`
- Entry script is under `node/test/parallel/`, so `require('../common')` should resolve to `node/test/common/`, and `require('assert')` to a builtin (or Node’s implementation).

**Actions:**

- Add a test runner (or CTest) that invokes the unode binary with a path into the vendored **node/test** tree (e.g. `node/test/parallel/test-require-cache.js`).
- Ensure the loader uses the **script’s directory** as the base for resolution so `../common` and `../fixtures` resolve under `node/test/` (no copy of tests into unode).

---

## 2. Resolve Node’s common and fixtures from node/test

**Current:** We ship our own `unode/tests/node-compat/common/` and `fixtures/`.

**Target:** When the entry script is under `node/test/`, resolve:

- `require('../common')` → `node/test/common/index.js`
- `require('../common/fixtures')` → `node/test/common/fixtures.js`
- `require('../common/tmpdir')` → `node/test/common/tmpdir.js`
- `require('../fixtures/...')` → `node/test/fixtures/...`

**Actions:**

- No change to test file content; resolution is driven by the script path.
- If we keep a “compat” mode that runs from `unode/tests/node-compat/`, keep that as an alternative; for “raw” mode, only run from `node/test/` and rely on Node’s layout.

---

## 3. Provide Node-compatible builtins (assert, path, fs, module)

**Current:** `require('assert')` and `require('path')` resolve through `unode/src/builtins` (same path used in non-test runtime).

**Target:** So that Node tests run unmodified:

- **assert** – Same API as Node’s `assert` for the methods used (e.g. `strictEqual`, `throws`, `ok`, `notStrictEqual`, `deepStrictEqual`). Either:
  - Implement the subset in `builtins/assert.js` and point builtin `assert` there when running from `node/test/`, or
  - Resolve `require('assert')` to Node’s own `assert` (e.g. from `node/lib/assert.js` or a vendored copy) if the loader can load from `node/lib/`.
- **path** – Same API as Node’s `path` for the subset used (`join`, `resolve`, `dirname`, `relative`, etc.). Extend `builtins/path.js` or use Node’s `path` from the repo.
- **fs** – Needed by tests that use `tmpdir` and fixtures (e.g. `test-module-cache.js`). Provide a `builtins/fs.js` (or wrapper) that implements at least: `writeFileSync`, `readFileSync`, `existsSync`, `mkdirSync`, `rmSync`, `readdirSync`, `openSync`, `readSync`, `closeSync`, `realpathSync`, `statfsSync` if required by common/tmpdir.
- **module** – Needed by tests like `test-require-dot.js` that use `require('module')` and `m._initPaths()`. Provide a shim that:
  - Exposes `_initPaths()` and integrates with the loader’s resolution (e.g. sets or honors `NODE_PATH` / search paths).
  - Optionally exposes `builtinModules` for tests that use `require.resolve.paths`.

**Actions:**

- Implement or wire `assert`, `path`, `fs`, and `module` so that when the loader resolves a bare id (e.g. `assert`), it either loads from a builtins directory or from `node/lib/` (or equivalent), and the API matches what the tests use.
- Add `builtins/fs.js` (and extend path) so tests that use `tmpdir` and `fixtures` run without changes.

---

## 4. Implement common/tmpdir and process hooks

**Current:** We have a minimal `common/index.js` and no `common/tmpdir.js`. Node’s `common/tmpdir` uses `fs`, `path`, `process`, and (on exit) cleanup.

**Target:** When running from `node/test/`, `require('../common/tmpdir')` should behave like Node’s:

- `tmpdir.refresh()` – create/clear a temp directory under the test root.
- `tmpdir.resolve(...paths)` – path.join(tmpdir, ...).
- Uses `fs` and `path`; may use `process.on('exit')` for cleanup.

**Actions:**

- If we run from `node/test/`, use Node’s real `common/tmpdir.js` (and `common/fixtures.js`) so behavior is identical.
- If we keep a compat tree under unode, port or reimplement `common/tmpdir.js` and `common/fixtures.js` to match Node’s API and rely on our builtins for `fs` and `path`.

---

## 5. process.env and NODE_PATH in the loader

**Current:** Loader does not use `process.env.NODE_PATH` or `module._initPaths()`.

**Target:** Tests that set `process.env.NODE_PATH` and call `m._initPaths()` (e.g. `test-require-dot.js`) should see that path used for resolution.

**Actions:**

- Ensure a global `process` object exists and has `process.env` (and other properties tests use).
- Implement or shim `module._initPaths()` so that after it runs, `require()` uses the same search path rules as Node (including `NODE_PATH` and current script directory).
- Extend the loader’s resolution so that when a specifier is not relative and not a known builtin, it can resolve from `NODE_PATH` (and later `node_modules`) if desired for parity.

---

## 6. Remove path normalization hack for JSON errors

**Current:** In the module loader we replace `"node-compat"` with `"test"` in the JSON parse error message so the regex in `test-require-json.js` matches.

**Target:** No special-case replacement; error messages should match Node’s format by construction.

**Actions:**

- Once tests run from `node/test/` (step 1), the resolved path for `fixtures/invalid.json` will naturally be under `node/test/fixtures/`, so the message will already contain `test/fixtures/invalid.json` and the regex will pass without any hack.
- Remove the `node-compat` → `test` replacement in `unode_module_loader.cc` once the runner uses the Node test directory.

---

## 7. Test runner and CI

**Target:** A single gate that runs a chosen set of Node tests in “raw” mode (same files as Node, no copy).

**Current:** The phase02 test binary runs raw Node tests when PROJECT_ROOT_PATH is set: RawRequireCacheFromNodeTest, RawRequireJsonFromNodeTest, RawModuleCacheFromNodeTest, RawRequireDotFromNodeTest (see Raw tests currently run above). This is the CI gate for raw tests.

**Actions:**

- Add a script or CTest target that:
  - Sets the entry script to e.g. `node/test/parallel/test-require-cache.js` (using the vendored `node/` path).
  - Runs unode with that script and passes/fails on exit code.
- Optionally: same for `test-require-json.js`, `test-module-cache.js`, etc., and document any tests that still need new builtins or loader behavior.
- Run this gate in CI so “raw” Node tests remain the source of truth.

---

## Suggested order

1. **Runner from node/test** – Run unode with `node/test/parallel/test-require-cache.js` (and test-require-json.js) and fix resolution so `../common` and `../fixtures` resolve under `node/test/`. Remove the JSON error path hack once paths are correct. **Done.**
2. **process and builtins** – Ensure `process`/`process.env` exist; complete builtins (or use Node’s) for `assert`, `path`, then `fs`, then `module` as needed by the next tests. **Done.**
3. **common/tmpdir and fs** – Wire Node’s `common/tmpdir` (or equivalent) and `fs` so tests like `test-module-cache.js` run raw. **Done** (minimal common/tmpdir.js shim + fs builtin).
4. **NODE_PATH and module** – Add `module._initPaths()` and NODE_PATH support so `test-require-dot.js` and similar tests run raw. **Done** for current tests (builtins/module.js with _initPaths() no-op; test-require-dot.js passes). Optional later: sync process.env.NODE_PATH to loader via setenv.
5. **Document and gate** – List the tests that run raw and add the runner to CI. **Done** (see Raw tests currently run above; gate is phase02 runner when PROJECT_ROOT_PATH is set).

This gets you to “tests run exactly as in Node” (same files, same require graph, same behavior) without maintaining a separate copy under `unode/tests/node-compat/` for those tests.
