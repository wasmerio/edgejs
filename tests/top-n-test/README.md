# top-n-test

Smoke tests for the top-100 most popular npm packages. Each package has a hand-written test that exercises the library the way a real developer would use it. The suite can run against multiple runtimes (Node.js, EdgeJS, WASIX EdgeJS) to verify compatibility.

## Updating the package list

The top-100 list is based on npm `last-year` downloads and is locked by default. To refresh it:

```bash
node scripts/fetch-top-packages.js --count=100 --candidate-limit=500 --allow-update
```

Without `--allow-update`, the script refuses to overwrite the locked `top-packages.json`.

## Running tests

Three runners are available via `-r` / `--runner`:

| Runner | Command | Env vars |
|---|---|---|
| Node.js | `node runner.js -r node --all` | — |
| EdgeJS | `node runner.js -r edgejs --all` | `EDGEJS_BIN` (default: `edgejs`) |
| WASIX EdgeJS | `node runner.js -r wasix_edgejs --all` | see below |

The `wasix_edgejs` runner uses `wasmer-dev` to run tests inside the EdgeJS WASIX runtime. Configurable env vars:

| Env var | Default | Description |
|---|---|---|
| `WASMER_BIN` | `wasmer-dev` | Path to the Wasmer binary |
| `EDGEJS_PACKAGE` | `wasmer/edgejs` | Wasmer package to run |
| `WASMER_REGISTRY` | `wasmer.io` | Wasmer registry URL |

### Run a single package

```bash
node runner.js -r node chalk
```

### Skip specific packages

```bash
node runner.js -r node --all --skip ws,express
```

### Continue past failures

By default the runner stops on the first failure. To run everything:

```bash
node runner.js -r node --all --no-fail-fast
```

### List available packages

```bash
node runner.js --list
```
