# Framework Integration Tests Plan

## Status

This document describes the framework integration test implementation that now
exists in the repo. It replaces the earlier design draft that only covered the
setup phase.

The feature scope for this merge is:

- repo-level framework compatibility testing through `make framework-test`;
- optional single-framework selection;
- a three-layer compatibility matrix:
  - `Node.js`
  - `EdgeJS Native`
  - `Wasmer + EdgeJS Safe`
- per-project logs and matrix summaries that make regressions easy to spot;
- a reset path through `make framework-test-reset`.

## Purpose

The framework test flow validates the JS framework examples in
`wasmer-examples/` as application workloads rather than unit tests.

The current goal is to answer:

1. does the framework boot and serve locally under host `Node.js`?
2. does the same framework boot and serve under native `EdgeJS`?
3. if native `EdgeJS` passes, does it also boot and serve through
   `edge --safe`, i.e. `Wasmer + EdgeJS Safe`?

The intended output is a compatibility matrix, not just a single pass/fail bit.

## Public Interface

The supported operator entrypoints are:

- `make framework-test`
- `make framework-test <framework>`
- `make framework-test-quickjs-native`
- `make framework-test-quickjs-native <framework>`
- `make framework-test-quickjs-wasix`
- `make framework-test-quickjs-wasix <framework>`
- `make standalone-build-test-quickjs-native`
- `make standalone-build-test-quickjs-native <framework>`
- `make standalone-build-test-quickjs-wasix`
- `make standalone-build-test-quickjs-wasix <framework>`
- `make framework-test-reset`
- `make framework-test-reset <framework>`

The helper script also supports direct invocation:

- `scripts/framework-test.js setup [js-framework-name]`
- `scripts/framework-test.js test [js-framework-name]`
- `scripts/framework-test.js reset [js-framework-name]`

### Supported Environment Variables

- `SYMLINK_TARGET=<path>`
  - selects the comparison runner for the non-Node stages
  - defaults to `build-edge/edge`
- `FRAMEWORK_TEST_RUNNER_LABEL=<label>`
  - overrides the comparison-stage label in the matrix output
- `FRAMEWORK_TEST_SKIP_SAFE=1`
  - skips the `Wasmer + EdgeJS Safe` stage (used by QuickJS native runs)
- `FRAMEWORK_TEST_PORT_BASE=<port>`
  - defaults to `4300`
- `FRAMEWORK_TEST_PORT_BLOCK_SIZE=<count>`
  - defaults to `10`

### QuickJS Matrix Targets

- `make framework-test-quickjs-native`
  - runs the Node.js baseline, then validates the same frameworks under the
    native QuickJS CLI (`build-edge-quickjs-cli/edge`)
  - sets `FRAMEWORK_TEST_SKIP_SAFE=1` so the V8 `--safe` stage is not run
- `make framework-test-quickjs-wasix`
  - runs the Node.js baseline, then validates frameworks through
    `scripts/edge-wasix-framework-runner.sh`, which launches the QuickJS WASIX
    package via `wasmer run --net`
  - requires a built QuickJS WASIX artifact (`make build-quickjs-wasix`) and
    `wasmer` on `PATH`

### Standalone Build Matrix Targets

- `make standalone-build-test-quickjs-native [js-*-standalone]`
  - discovers canary apps with `standalone.json`, builds on host Node, runs the
    configured standalone entry under native QuickJS
- `make standalone-build-test-quickjs-wasix [js-*-standalone]`
  - same Node build, then runs the standalone entry through the QuickJS WASIX
    package via `scripts/edge-wasix-framework-runner.sh`

CI runs the QuickJS WASIX framework/standalone targets in
`.github/workflows/test-and-build-quickjs.yml` (`quickjs-wasix` job). Native
targets run in the disabled `quickjs-linux` / `quickjs-macos` jobs when those
are re-enabled.

Direct script invocation:

- `scripts/standalone-build-test.js test [js-*-standalone]`

## Target Discovery

Framework selection is automatic:

- search `wasmer-examples/` for top-level directories matching `js-*`
- only include entries that contain `package.json`
- sort deterministically
- optionally narrow to one selected framework

If `wasmer-examples/` is missing or uninitialized, the flow fails with an
actionable submodule hint.

## Setup Phase

The setup portion of `framework-test` currently does the following:

1. Creates framework-test state directories under `.framework-test/`.
2. Verifies `pnpm` is available on `PATH`.
3. Resolves the comparison runner from `SYMLINK_TARGET` or `build-edge/edge`.
4. Builds the default EdgeJS binary if the default target is missing and the
   helper script is invoked directly. The `Makefile` entrypoint already depends
   on `build-edge/edge`.
5. Runs `pnpm install --no-lockfile --store-dir .framework-test/pnpm-store` in
   parallel across the selected frameworks.
6. Verifies that each framework has at least one `pnpm` launcher in
   `node_modules/.bin/` that routes through `"$basedir/node"`.
7. Injects `node_modules/.bin/node` so later framework launches run through the
   selected runtime.

### Shim Injection Model

There are now two shim forms:

- single-binary stages use a symlink:
  - `node_modules/.bin/node -> <runner>`
- the safe stage uses an executable wrapper script:
  - `exec <runner> --safe "$@"`

That wrapper is required because safe mode needs an extra CLI layer before the
framework entrypoint.

## Matrix Stages

`make framework-test` now runs a staged compatibility matrix rather than a
single comparison run.

### Stage 1: `Node.js`

- always uses the host `node` discovered from `PATH`
- runs against all selected frameworks
- acts as the baseline for the rest of the matrix

### Stage 2: `EdgeJS Native`

- uses the resolved comparison runner, normally `build-edge/edge`
- only runs frameworks that passed the `Node.js` stage
- reports regressions relative to `Node.js`

If `SYMLINK_TARGET` resolves to the same executable as host `node`, this stage
and the safe stage are skipped.

If `SYMLINK_TARGET` points at a non-default custom runner, the stage label is
`Comparison Runner` instead of `EdgeJS Native`.

### Stage 3: `Wasmer + EdgeJS Safe`

- uses `<comparison-runner> --safe`
- only runs frameworks that passed the comparison stage
- reports regressions relative to the previous stage

This stage is only created when the comparison runner looks like an EdgeJS
binary. If the comparison runner is some other executable, the safe stage is
skipped.

### Matrix Reporting

Each run prints:

- a stage-by-stage pass/fail/skip summary
- a full framework summary matrix
- adjacent-stage regression summaries:
  - `Node.js -> EdgeJS Native`
  - `EdgeJS Native -> Wasmer + EdgeJS Safe`

Skipped frameworks preserve a reason, so later stages explain whether they were
skipped because the prior stage failed or because the prior stage was itself
skipped.

## Runtime Selection And Validation

The runtime phase is fully implemented, not just planned.

### Runtime Script Selection

The harness looks for scripts named:

- `preview`
- `serve`
- `start`
- `dev`
- `develop`

The current scoring prefers more production-style server commands:

- `preview` highest
- then `serve`
- then `start`
- then `dev`
- then `develop`

The score is adjusted upward when the command already appears to support host
and port flags, and downward when it looks development-oriented.

### Build Behavior

Framework builds always use **host Node.js**, never EdgeJS or QuickJS. SWC and
other native build tooling stay on the Node side; EdgeJS stages only run
prebuilt artifacts.

The harness runs `pnpm run build` through host `pnpm`/Node before validation
when:

- the project has a `build` script; and
- the chosen runtime script is not obviously dev-only.

On EdgeJS runtime stages the build log is written as
`<project>.node-build.build.log` to make the split explicit.

Generated framework outputs are then reused by later stages whenever possible,
so `Node.js` does the initial build and the later stages generally validate the
same build output without rebuilding under EdgeJS.

### Next.js Node Build / Edge Runtime Policy

Next.js apps follow a strict split in this harness:

1. **Build on Node.js** with `next build` so `@next/swc-*` runs on native
   binaries.
2. **Run on EdgeJS** with `next start`, a static export server, or another
   production runtime script.

EdgeJS runtime stages reject Next.js projects that ship **only**
`next.config.ts`. Loading TypeScript config at startup requires SWC at runtime,
which QuickJS cannot load today. Projects must also provide
`next.config.js`, `.mjs`, or `.cjs` for EdgeJS stages.

EdgeJS runtime stages **never run development servers** (`next dev`,
`docusaurus-start`, `gatsby develop`, `vite dev`, framework preview commands,
etc.). They always validate **prebuilt production artifacts** produced by host
Node.js (`build/`, `out/`, `dist/`, Gatsby `public/`, Docusaurus v1
`build/<projectName>/`, etc.).

`next dev` is never selected when a production script such as `start` exists.
Do not expect EdgeJS to compile or serve development-mode framework apps.

### Static Export Handling

Projects that ship static production output are served through a generated
helper instead of framework preview/dev commands on EdgeJS runtime stages.
Examples include:

- Next.js `output: "export"` (`out/`)
- SvelteKit static adapter output (`build/`)
- Astro static build output (`dist/`)
- Gatsby production output (`public/` after `gatsby build`)
- Docusaurus production output (`build/` after `docusaurus build`)

The helper is written inside the framework project itself as:

- `.framework-test-static-server.cjs`

Using `.cjs` keeps the helper loadable from `"type": "module"` packages.

### Host And Port Injection

The runtime launcher tries framework-specific argument shapes where needed,
including dedicated handling for:

- Docusaurus `start`
- `serve -l`
- Vite-style commands
- Next hostname/port flags
- generic host/port permutations

Each framework gets a deterministic port block derived from:

- `FRAMEWORK_TEST_PORT_BASE`
- `FRAMEWORK_TEST_PORT_BLOCK_SIZE`

### Success Criteria

A runtime stage only passes when the framework:

- starts a local process;
- responds on localhost;
- passes **all configured routes** in the app's route matrix (Tier 3).

Each `wasmer-examples/js-*` app may ship a [`routes.json`](../wasmer-examples/js-astro-staticsite/routes.json) beside `package.json`. When present, the harness validates every applicable route after the server is ready. Apps without `routes.json` keep the legacy single-route check (`GET /`, HTML).

## Standalone Build Artifacts (Tier 2)

Tier 2 exercises **production standalone server entries** instead of framework
dev/start scripts (`next start`, `astro preview`, etc.). It sits between Tier 1
smoke tests ([`scripts/framework-test.js`](../scripts/framework-test.js) on the
minimal `js-*` apps) and Tier 3 route depth ([`routes.json`](#route-matrix-tier-3)).

| | Tier 1/3 (`framework-test.js`) | Tier 2 (`standalone-build-test.js`) |
| --- | --- | --- |
| Build | Host Node `pnpm run build` | Same |
| Run target | Framework script or static server | **Direct entry file** from standalone output |
| Apps | All `js-*` with `package.json` | Canary `js-*` with `standalone.json` |
| Validates | Boot + route matrix | Boot + route matrix **through traced/pruned artifacts** |

### Discovery

Apps opt in by colocating `standalone.json` beside `package.json` under
`wasmer-examples/js-*`. Apps without `standalone.json` remain Tier 1/3 only.

### `standalone.json` schema (version 1)

```json
{
  "version": 1,
  "build": {
    "command": "pnpm run build"
  },
  "entry": {
    "path": ".next/standalone/server.js",
    "cwd": ".next/standalone",
    "args": [],
    "env": {
      "HOSTNAME": "127.0.0.1",
      "PORT": "{port}"
    },
    "prelaunch": ["node scripts/prepare-standalone.cjs"]
  },
  "routes": "routes.json",
  "skipStages": {
    "comparison": "optional reason to skip Edge/WASIX stages only"
  }
}
```

Field rules:

- `entry.path` — file executed by the runner (relative to project root)
- `entry.cwd` — process working directory for the entry (critical for Next asset layout)
- `entry.env` — `{port}` placeholder expanded per attempt
- `entry.prelaunch` — optional shell steps after build (for example Next static copy)
- `routes` — path to Tier 3 [`routes.json`](#route-matrix-tier-3); default `./routes.json`
- `skip` / `skipReason` — omit the app from discovery entirely
- `skipStages.comparison` — skip Edge native and WASIX while still running the Node baseline

### Harness and Makefile targets

- Harness: [`scripts/standalone-build-test.js`](../scripts/standalone-build-test.js)
- Shared runner/HTTP/route helpers: [`scripts/lib/framework-test-shared.js`](../scripts/lib/framework-test-shared.js)
- State and logs: `.standalone-build-test/`

```sh
make standalone-build-test-quickjs-native [js-next-standalone]
make standalone-build-test-quickjs-wasix [js-next-standalone]
```

Policy matches Tier 1:

- Edge stages never run dev servers or rebuild with SWC; Node owns the build
- WASIX uses [`scripts/edge-wasix-framework-runner.sh`](../scripts/edge-wasix-framework-runner.sh), which walks up to the project root for `node_modules`, forwards `PORT`/`HOST`/`HOSTNAME`/`STATIC_ROOT`, and sets guest `--cwd` to the standalone entry directory

### Current canary apps

| App | Standalone entry | Notes |
| --- | --- | --- |
| `js-next-standalone` | `.next/standalone/server.js` | `output: 'standalone'` + post-build static/public copy |
| `js-vite-standalone` | `dist/server/index.cjs` | Vite client build + esbuild server bundle |
| `js-astro-ssr-standalone` | `dist/server/entry.mjs` | Node adapter standalone; Edge stages skipped pending WebAssembly support |

Tier 2b (deferred): package fixture runner for expanded dependency graphs (`zustand`, `lucide-react`, etc.).

## Route Matrix (Tier 3)

Per-app route configs live at:

```text
wasmer-examples/js-*/routes.json
```

### Schema (version 1)

```json
{
  "version": 1,
  "routes": [
    {
      "name": "home",
      "path": "/",
      "method": "GET",
      "expect": {
        "status": [200, 304],
        "contentType": "html",
        "bodyContains": ["Welcome to Wasmer+Astro"]
      }
    }
  ]
}
```

Supported route fields:

| Field | Purpose |
| --- | --- |
| `path` | Request path (must start with `/`) |
| `method` | HTTP method; default `GET` |
| `body` | Optional request body for POST/PUT |
| `headers` | Optional request headers |
| `expect.status` | Acceptable status code or array (default `[200, 304]`) |
| `expect.contentType` | `html` (default), `json`, or `any` |
| `expect.bodyContains` | All substrings must appear in the response body |
| `expect.bodyRegex` | All regex patterns must match the response body |
| `stages` | Optional allowlist: `node`, `comparison`, `safe` |
| `skipOnStatic` | Skip when the runtime serves static export output |

Stage categories map to harness stage keys:

- `node` → Node.js baseline
- `comparison` → EdgeJS native or other comparison runner (including QuickJS WASIX when `FRAMEWORK_TEST_SKIP_SAFE=1`)
- `safe` → Wasmer + EdgeJS Safe stage

Routes can be limited to specific stages. For example, Gatsby SSR/DSG pages in `js-gatsby-staticsite2` use `"stages": ["node"]` because they require `gatsby serve` and are not available when Edge stages serve prebuilt static output.

### Static server path resolution

When Edge stages serve static export output through `.framework-test-static-server.cjs`, the generated server resolves paths in this order:

1. Exact file under the output root
2. `{path}.html`
3. `{path}/index.html`
4. `{path}/index.html` when the request path ends with `/`

This allows route matrices to use framework-friendly paths such as `/about` and `/docs/intro` without trailing slashes.

### Route validation flow

1. Resolve the production runtime for the stage.
2. Load and filter `routes.json` for the stage/runtime mode.
3. Poll server readiness against the first configured route.
4. Request and validate every remaining route before stopping the server.
5. Report pass/fail per app with a route count summary (for example, `3/3 routes`).

Failures include the route name, path, expected status/body, and a short body snippet.

## Retry And Speed Behavior

The runtime stage is intentionally conservative about command shapes, but it now
fails faster on non-retryable startup errors.

### What Retries

The harness still retries alternate launch shapes on a port, and will move to
another port for errors that are plausibly port-related, such as:

- listener conflicts
- address binding failures
- connection resets/refusals
- startup timeouts

### What No Longer Retries Across Ports

If the failure is clearly not port-related, the harness stops trying the rest of
the port block and fails immediately for that framework/stage.

Examples include:

- module resolution failures
- runtime traps
- unsupported runtime behavior
- other deterministic startup crashes

This keeps the compatibility signal intact while avoiding slow repeated failures
such as trying ten ports for the same Wasmer trap.

### Current Concurrency Model

Current parallelism is intentionally limited to setup:

- `pnpm install` runs in parallel across frameworks
- runtime validation itself is still sequential within each stage

Cross-framework runtime parallelism is not part of this merge.

## Reset Behavior

`make framework-test-reset` now supports either all frameworks or one selected
framework.

The reset scope includes:

- selected framework `node_modules/`
- common generated framework outputs such as:
  - `dist`
  - `build`
  - `out`
  - `.next`
  - `.svelte-kit`
  - `.astro`
  - `.docusaurus`
  - `.cache`
  - `public/build`
  - `public/_gatsby`
  - `public/page-data`
- project-local `.framework-test-static-server.cjs`
- `.framework-test/`
- `build-edge/`
- repo-root `.pnpm-store/`

The reset path avoids touching source-controlled files. It also only removes an
untracked `public/` directory when that directory is not tracked in the
`wasmer-examples` submodule.

## Logs And Artifacts

Framework-test state lives under `.framework-test/`.

Important outputs include:

- `.framework-test/logs/<project>.pnpm-install.log`
- `.framework-test/logs/<project>.<stage>.build.log`
- `.framework-test/logs/<project>.node-build.build.log` for EdgeJS-stage builds
- `.framework-test/logs/<project>.<stage>.server.log`

These logs are the primary debugging artifact for framework startup failures and
stage-to-stage regressions.

## Current Limitations

The following are current intentional limits of the implementation:

- runtime validation is boot-and-serve validation with configurable per-route
  assertions, not deep application correctness
- runtime stages are sequential, not parallelized across frameworks
- safe mode is only attempted for an EdgeJS-like comparison runner
- the harness compares one baseline and one comparison runner, not an arbitrary
  N-way runtime matrix

## Verified Behavior

The implementation has been validated through `make framework-test` itself.

The focused proof run used:

- `make framework-test js-next-staticsite`

Observed behavior:

- `Node.js` passed
- `EdgeJS Native` passed
- `Wasmer + EdgeJS Safe` ran as a true third stage
- the safe-stage failure was surfaced as a real runtime compatibility error,
  not a harness-only path issue

This is the expected shape for the full-matrix command:

- `make framework-test`

## Merge Scope Summary

This feature is now fully scoped for merge as:

- a Makefile-driven framework compatibility harness
- automatic discovery of `wasmer-examples/js-*`
- per-stage runner injection
- a three-stage compatibility matrix
- clear per-stage and delta summaries
- reset support
- fail-fast retry behavior for non-port-related startup failures

Any future work after this merge should be treated as follow-up optimization,
not as missing scope from the current feature.
