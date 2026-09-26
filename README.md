<p align="center">
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset="./assets/edgejs-logo-white.svg">
    <source media="(prefers-color-scheme: light)" srcset="./assets/edgejs-logo-dark.svg">
    <img src="./assets/edgejs-logo-dark.svg" alt="Edge.js logo" height="100">
  </picture>
</p>

<p align="center">
  Run JavaScript anywhere. <b>Safely</b>.
</p>

<hr />

Edge.js is a secure **JavaScript** runtime, designed for Edge computing and AI workloads.

Edge.js **uses WebAssembly** for sandboxing when in `--safe` mode, so even the most insecure programs can run on it safely. Edge also is:

* ✅ Fully **compatible with Node.js**.
* 🔒 **Sandboxed** by design.
* 🧩 Pluggable with any **JS engine**: V8, JavaScriptCore or QuickJS.
* 💪 Compatible with **any package manager**: NPM/PNPM/Yarn/Bun.

## Install Edge.js

```bash
curl -fsSL https://edgejs.org/install | bash
```

## Use it!

You can use it as you would do it with Node.js:

```js
const http = require("node:http");

http
  .createServer((_req, res) => {
    res.end("hello from edge\n");
  })
  .listen(3000, () => {
    console.log("listening on http://localhost:3000");
  });
```

```bash
$ edge server.js
```

If you want to use it in your current workflow, just wrap your commands with `edge`:

```bash
$ edge node myfile.js
$ edge npm install
$ edge pnpm run dev
```

## Development

First, clone the repo and initialize only the `napi` submodule:
```bash
git clone --recurse-submodules https://github.com/wasmerio/edgejs.git
cd edgejs
```

Build the CLI locally:

```bash
make build
./build-edge/edge server.js
```

```bash
./build-edge/edge --run dev
```

Or run the tests:
```bash
make test
NODE_TEST_RUNNER="$(pwd)/build-edge/edge" \
./test/nodejs_test_harness --category=node:assert
```

### Host-JavaScript WASIX package

The root [`wasmer.toml`](./wasmer.toml) describes the engine-free
`syrusakbary/edgejs` package. Its WebAssembly module imports the standard N-API
surface from `napi` and the EdgeJS extension surface from
`napi_extension_wasmer_v0`; it does not embed QuickJS or V8. A compatible host
provides those imports using its own JavaScript engine.

```bash
make build-wasix
make validate-wasix-imports
wasmer package build --check .
```

The build runs the import/engine validation automatically. The embedded
QuickJS package remains separately available under `quickjs-wasm/`.

Both WASIX packages expose `npm` as an alias for the bundled pnpm CLI.
They retain real npm as `edge-npm-internal` and configure pnpm's `npm-path`
to use it for delegated operations, avoiding recursive calls to the alias.

This host-engine mode is experimental. The buffer ownership and synchronization
contract is documented in
[`004_buffer_ownership.md`](./plans/quickjs-wasm/development/dev_007_host_js_imported_napi/004_buffer_ownership.md),
and deferred security work is recorded in
[`SECURITY-HOST-JS-NAPI.md`](./SECURITY-HOST-JS-NAPI.md). The browser regression
gate covers package installation, timers, promises, workers, networking, HTTP,
and a rendered Next.js development preview.

## Contribute 🤗

We have created a [public ROADMAP](https://github.com/wasmerio/edgejs/issues/8), so you can contribute into the project easily!

- `0.x` Production readiness: platform coverage across Linux, Windows, macOS, iOS, and Android; reliability in constrained environments; security audits; and successful real production use.
- `1.x` Need for speed: faster startup, faster core paths, and performance that competes with or beats Node.js, Bun, and Deno on most workloads.
- `2.x` Enhancements: first-class TypeScript support and a smoother developer experience.

For architecture detail, see [`ARCHITECTURE.md`](./ARCHITECTURE.md).
