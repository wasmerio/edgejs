<p align="center">
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset="./assets/edgejs-logo-white.svg">
    <source media="(prefers-color-scheme: light)" srcset="./assets/edgejs-logo-dark.svg">
    <img src="./assets/edgejs-logo-dark.svg" alt="Edge.js logo" height="120">
  </picture>
</p>

<p align="center">
  Run JavaScript anywhere. Safely.
</p>

<hr />

Edge.js is a **Node.js-compatible** runtime with stronger isolation and simpler architecture than Node. No new platform to learn: just your
Node.js code, `node_modules`, `package.json`, `node:` built-ins, and N-API addons.

- **Full Node.js compatibility** Use your codebase, packages, and workflow.
- **Safer by design.** Built for edge, embedded, serverless, and multi-tenant workloads.
- **N-API is the boundary.** Native compatibility matters; engine details stay behind stable interfaces.
- **Compatible with NPM/PNPM/Yarn/Bun installers.** use your current package manager with `ubi`.


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

## Development

Build the CLI locally:

```bash
make build
./build-ubi/ubi server.js
```

```bash
./build-ubi/ubi --run dev
```

Or run the tests:
```bash
make test
NODE_TEST_RUNNER="$(pwd)/build-ubi/ubi" \
./node-test/nodejs_test_harness --category=node:assert
```


## Roadmap

### [Contribute to our ROADMAP](https://github.com/wasmerio/ubi/issues/8)

- `0.x` Production readiness: platform coverage across Linux, Windows, macOS, iOS, and Android; reliability in constrained environments; security audits; and successful real production use.
- `1.x` Need for speed: faster startup, faster core paths, and performance that competes with or beats Node.js, Bun, and Deno on most workloads.
- `2.x` Enhancements: first-class TypeScript support and a smoother developer experience.

For architecture detail, see [`ubi/README.md`](./ubi/README.md) and [`ubi/ROADMAP.md`](./ubi/ROADMAP.md).
