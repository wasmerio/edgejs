# Tier 2 Standalone Build Artifact Tests

| | | Remarks |
| --- | --- | --- |
| **Status** | 🟢 | Harness, canary apps, Makefile targets, and verification complete. |
| **Severity** | Low | Astro Edge/WASIX comparison stage remains intentionally skipped. |

## Goal

Exercise real production dependency graphs through framework-standard **standalone
server entries**, not `pnpm run dev` or generic framework start scripts. Tier 2
sits between Tier 1 smoke ([`scripts/framework-test.js`](../../../scripts/framework-test.js))
and Tier 3 route matrices ([`routes.json`](../../../wasmer-examples/js-svelte/routes.json)).

## Implementation

| Component | Path |
| --- | --- |
| Shared harness helpers | [`scripts/lib/framework-test-shared.js`](../../../scripts/lib/framework-test-shared.js) |
| Tier 2 harness | [`scripts/standalone-build-test.js`](../../../scripts/standalone-build-test.js) |
| WASIX runner (standalone cwd + env) | [`scripts/edge-wasix-framework-runner.sh`](../../../scripts/edge-wasix-framework-runner.sh) |
| Operator docs | [`plans/framework-integration-tests.md`](../../framework-integration-tests.md) |

Makefile targets:

```sh
make standalone-build-test-quickjs-native [selector]
make standalone-build-test-quickjs-wasix [selector]
```

## Canary app inventory

### `js-next-standalone`

- Next App Router with `output: 'standalone'` in `next.config.js`
- Post-build hook copies `.next/static` and `public` into `.next/standalone/`
- Entry: `.next/standalone/server.js`, cwd `.next/standalone`
- Routes: `GET /` with `Next standalone canary` body marker

### `js-vite-standalone`

- Vite multi-page client build (`index.html`, `about.html`) to `dist/`
- App-owned static server in `server/index.js`, bundled with esbuild to `dist/server/index.cjs`
- Entry cwd `dist`, `STATIC_ROOT=.`
- Routes: `/` and `/about.html`

Note: `vite-plugin-standalone@^3` is not published on npm; this canary uses Vite
build + esbuild bundling instead (see
[`007_framework_standalone_builds.md`](007_framework_standalone_builds.md)).

### `js-astro-ssr-standalone`

- Astro `output: 'server'` with `@astrojs/node({ mode: 'standalone' })`
- Entry: `dist/server/entry.mjs` (no esbuild CJS bundle; adapter uses top-level await)
- Routes: `GET /` with `Astro SSR standalone canary` marker
- **`skipStages.comparison`**: QuickJS lacks `WebAssembly` required by Astro designed error pages chunk

## Skip policy

- Project-level `"skip": true` removes the app from Tier 2 discovery.
- `"skipStages": { "comparison": "<reason>" }` keeps the Node baseline while skipping Edge native and WASIX until runtime blockers are fixed.

## Verification (2026-06-24)

### Native QuickJS

```sh
make standalone-build-test-quickjs-native
```

| App | Node.js | EdgeJS QuickJS Native |
| --- | --- | --- |
| `js-next-standalone` | PASS (1/1 routes) | PASS (1/1 routes) |
| `js-vite-standalone` | PASS (2/2 routes) | PASS (2/2 routes) |
| `js-astro-ssr-standalone` | PASS (1/1 routes) | SKIP (WebAssembly) |

### WASIX

```sh
make build-quickjs-wasix   # prerequisite
make standalone-build-test-quickjs-wasix
```

| App | Node.js | EdgeJS QuickJS WASIX |
| --- | --- | --- |
| `js-next-standalone` | PASS (1/1 routes) | PASS (1/1 routes) |
| `js-vite-standalone` | PASS (2/2 routes) | PASS (2/2 routes) |
| `js-astro-ssr-standalone` | PASS (1/1 routes) | SKIP (WebAssembly) |

WASIX runner changes required for standalone entries:

- Walk up from entry cwd to locate `node_modules` at the project root
- Map guest `--cwd` to the standalone subdirectory under the mounted project root
- Forward `PORT`, `HOST`, `HOSTNAME`, `STATIC_ROOT`, and `NODE_ENV` into the Wasmer guest

## CI

The QuickJS workflow (`.github/workflows/test-and-build-quickjs.yml`) runs these
targets in the active `quickjs-wasix` job after `make test-wasix-quickjs-only`:

- `make framework-test-quickjs-wasix`
- `make standalone-build-test-quickjs-wasix`

The disabled `quickjs-linux` and `quickjs-macos` jobs run the native
framework/standalone targets when those jobs are re-enabled.

Framework setup runs `pnpm install` serially by default (set
`FRAMEWORK_TEST_PARALLEL_PNPM=1` to restore parallel installs). Failed installs
print the last 40 lines of each fixture log in CI output. WASIX framework targets
depend on `build-quickjs-wasix/edgejs.wasm` rather than re-running a full rebuild
when the artifact already exists from an earlier workflow step.

## Known follow-ups (Tier 2b — deferred)

- [`scripts/package-fixture-test.js`](../../../scripts/package-fixture-test.js) for npm package import fixtures (`zustand`, `lucide-react`, `relay-runtime`, `depd`, etc.)
- Expand Astro canary with React + zustand + lucide once fixtures exist
- Re-enable Astro comparison stages when QuickJS provides WebAssembly or Astro drops the WASM dependency from designed error pages
