# Edge.js QuickJS WASIX

This package builds Edge.js as a WASIX WebAssembly binary with QuickJS compiled
into the guest module as the N-API provider.

## Build

```sh
quickjs-wasm/build.sh
```

The build uses `build-quickjs-wasix/` and writes:

```text
build-quickjs-wasix/edge.wasm
build-quickjs-wasix/edgejs.wasm
```

The build script verifies that the final wasm module does not import N-API
symbols such as `napi_*`, `node_api_*`, or `unofficial_napi_*`.

## Run

Use Wasmer 7.4.2 with a 4 MiB host stack. With LLVM on AMD64, Wasmer's default
1 MiB stack can run out before QuickJS can raise a catchable JavaScript stack
overflow. QuickJS's own linear-memory stack guard remains enabled. The Node
compatibility and framework launchers use 4 MiB by default; `WASMER_STACK_SIZE`
overrides it. Package annotations cannot set this host limit, so direct Wasmer
invocations need the option shown below.

```sh
wasmer run --stack-size 4194304 quickjs-wasm -- --version
wasmer run --stack-size 4194304 quickjs-wasm -- -e "console.log('hello from quickjs')"
```

The `edge` command remains the default entrypoint. The package also exposes
`node` as an alias for it.

## pnpm

Both `pnpm` and `npm` run the pinned pnpm 10.34.5 pure-JavaScript CLI. The build
stages pnpm's bundle and package-import worker with verified checksums. A small
package-local launcher adapts unsupported WASIX filesystem metadata operations;
it does not change EdgeJS's global `fs` behavior. Arguments after `--` are
forwarded to pnpm:

```sh
wasmer run --stack-size 4194304 quickjs-wasm --command=pnpm --volume=. -- --version
```

Run an install from the project directory with networking enabled and mount
that directory read-write into the guest:

```sh
wasmer run --stack-size 4194304 quickjs-wasm --command=pnpm --net --volume=. -- install react
```

`--command=pnpm` selects the pnpm entrypoint, `--net` permits registry access,
and the `--volume=.` shorthand exposes the current project as pnpm's writable
working directory. The separator `--` ends Wasmer's options; everything after
it is a pnpm argument. Commands that do not need registry access may omit
`--net`.

The WASIX command uses pnpm's hoisted linker and copy import method. This makes
installed dependencies real directories that EdgeJS can resolve and avoids a
Wasmer 7.2.1 linked-inode rename failure. Its content-addressed store is kept at
`/tmp/.pnpm-store` inside the guest. The store is intentionally ephemeral and
does not assume the mounted project is available at `/home`. The packaged
command also disables pnpm's update-available notification through
`npm_config_update_notifier=false`.

Run the end-to-end package smoke test with:

```sh
make test-wasix-pnpm WASMER_BIN=wasmer WASIX_PACKAGE_DIR="$PWD/quickjs-wasm"
```

It stages pnpm, installs pinned React in a temporary mounted project, checks the
manifest, lockfile, and ephemeral store path, confirms that the update
notification stays hidden, and resolves the installed package through Edge
QuickJS. Real npm is included as `edge-npm-internal` for pnpm's delegated
configuration and publish operations. Both public commands set pnpm's
`npm-path` to that entrypoint so delegation cannot recurse into the npm alias.
