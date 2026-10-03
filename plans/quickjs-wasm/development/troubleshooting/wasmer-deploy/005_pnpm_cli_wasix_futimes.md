# Wasmer Deploy: pnpm CLI WASIX `futimes` Fallback

| | | Remarks |
| --- | --- | --- |
| **Status** | 🟠 | `install react` completes through scoped command adaptations; native WASIX metadata and linked-inode limitations remain. |
| **Severity** | High | The packaged pnpm command cannot complete `install react` without a narrow compatibility fallback. |

## Issue

The pinned pure-JavaScript pnpm 10 bundle starts in both native Edge QuickJS
and QuickJS WASIX. Under WASIX, commands that initialize or update pnpm's
store first fail with:

```text
ENOSYS: function not implemented, futime
```

The exact install repro is:

```sh
wasmer run --net \
  --env HOME=/tmp \
  --volume=/private/tmp/edge-pnpm-project:/home \
  --volume=/private/tmp/pnpm-package:/pnpm \
  build-quickjs-wasix/edgejs.wasm -- \
  /pnpm/pnpm.cjs install react
```

The failure is reported from `node:fs` while pnpm's bundled `touch` code calls
`fs.futimes()` on an open store file. The same bundle reports its version and
shows help successfully. Native QuickJS also completes the pnpm startup and
help paths, so there is no native crash to inspect in LLDB; the failing syscall
is specific to the WASIX host/libc path.

After containing `futimes`, the install reaches pnpm's import worker. The
published pnpm package requires its adjacent `dist/worker.js`; staging only
`pnpm.cjs` is sufficient for help but not installation. With the worker staged,
pnpm's automatically selected hardlink import path exposes a Wasmer 7.2.1
inode bookkeeping panic in `path_rename.rs` when a linked store inode is moved
into place. Forcing pnpm's supported `package-import-method=copy` avoids that
panic and materializes React successfully.

The remaining manifest writes use `write-file-atomic`: WASIX leaves the
temporary `package.json.<id>` and `pnpm-lock.yaml.<id>` files in place and the
Edge process exits 127 before the final regular-file rename. A minimal WASIX
repro shows `fs.chown()` aborting the guest instead of returning an error;
`fs.chmod()` and `fs.fsync()` both complete. pnpm reads the existing manifest's
ownership and asks `write-file-atomic` to preserve it. The adapter therefore
needs to suppress ownership changes for this command before applying its
regular-file copy-and-unlink rename; directory renames must continue using the
runtime and must not be emulated broadly.

With ownership suppression added, `install react` exits 0 and persists the
manifest and lockfile. The isolated linker creates the virtual-store package
but does not leave the required top-level `node_modules/react` link, so the
result cannot resolve `require("react")`. The command must select pnpm's
supported hoisted linker under WASIX. Store, import-method, and linker settings
must be passed as `npm_config_*` environment configuration, not fixed CLI
options, because fixed install-only options make commands such as `store path`
reject the argument.

## Action Plan

1. Preserve Node's public `fs.futimes()` error semantics in the shared runtime.
2. Add a pnpm-command-local JavaScript adapter rather than turning every WASIX
   `futime` failure into global success.
3. In that adapter, intercept only `ENOSYS` from asynchronous and synchronous
   `fs.futimes`; propagate every other error unchanged. Timestamps are a
   best-effort metadata update for pnpm's store touch path.
4. Make `chown`, `fchown`, and `lchown` (callback, synchronous, and promise
   forms) successful no-ops within the pnpm main process and worker. WASIX does
   not expose a usable ownership change here, and allowing the call through
   aborts the guest rather than returning a catchable `ENOSYS`.
5. For pnpm's atomic regular-file replacement, implement `rename` as
   copy-overwrite followed by unlink when the source is a regular file. Keep
   directory and non-file renames on the original runtime path.
6. Stage pnpm's adjacent `worker.js` with an independent checksum, keep the
   content store inside the mounted project, and force pnpm's supported `copy`
   import method so Wasmer never creates linked inodes that later panic during
   rename bookkeeping.
7. Select pnpm's hoisted linker so installed dependencies are real,
   directly-resolvable directories instead of missing virtual-store links.
   Supply these three pnpm settings through `npm_config_*` environment entries
   so non-install commands do not receive unknown fixed arguments.
8. Load the pinned upstream pnpm bundle after the adapter is installed, keeping
   `process.argv` unchanged.
9. Verify `--version`, `store path`, an empty install, and `install react` in a
   fresh mounted project. Inspect any subsequent failure separately before
   broadening the adapter.
10. Re-run existing `edge` and `node` package command smokes to ensure their
   behavior is untouched.

## Scope Boundary

This workaround belongs to the packaged pnpm command. It must not change the
shared EdgeJS fs binding or claim that WASIX implements descriptor timestamp
updates. A future WASIX/libc implementation of `futimens` can remove the
adapter once the same install smoke passes without it.

## Current Status and Validation

The final command stages the upstream CLI and worker bundles and runs them
through `edge-pnpm.cjs` / `wasix-fs.cjs`. The Wasmer command supplies the
hoisted linker, copy importer, and project-local store through environment
configuration. This avoids fixed command options that break `pnpm store path`.

Verified with Wasmer 7.2.1:

```sh
make test-wasix-pnpm WASMER_BIN=wasmer
```

The smoke test checks pnpm 10.34.5, installs React 19.2.8 from the registry in
a fresh mounted project, verifies `package.json` and `pnpm-lock.yaml`, asserts
that `node_modules/react` is a real directory, and resolves the installed
version through the packaged Edge QuickJS command. The target passes.

## 2026-10-02: private npm subprocess entrypoint

GitHub Actions run `37078796961`, job `111074548689`, failed in
`make test-wasix-pnpm` while executing the public alias's delegated
`npm config get registry` command. The exact failure reproduced locally with
Wasmer 7.4.2:

```text
Error: Cannot find module '/home/config'
```

pnpm starts `/bin/edge-npm-internal config get registry` with `spawnSync`.
When this private package command is launched as a subprocess, its manifest
`main-args = ["/npm/bin/npm-cli.js"]` are not prepended. Edge therefore treats
`config` as its script. A diagnostic confirmed that explicitly starting
`process.execPath /npm/bin/npm-cli.js config get registry` reaches npm.

Action plan: keep this adaptation in the shared pnpm launcher, translate only
the private npm fallback on WASIX, preserve spawn options and arguments, and
verify through the existing package smoke. There is no native crash to inspect
with LLDB or shared runtime semantic change to make.

`quickjs-wasm/pnpm/edge-pnpm.cjs` now wraps `child_process.spawnSync` before
loading pnpm. For the exact `/bin/edge-npm-internal` target on `process.platform
=== 'wasi'`, it starts the interpreter with npm's explicit script entrypoint.
Both standard and QuickJS manifests mount this same launcher. Public npm/pnpm
aliases still run pnpm, and unrelated subprocesses retain their original
behavior.

Verified with the existing embedded QuickJS WASIX artifact and Wasmer 7.4.2:

```sh
python3 scripts/test-wasix-pnpm.py --package-dir quickjs-wasm --timeout 90
```

The complete test passes: both command versions, delegated project `.npmrc`
registry lookups, store paths, React online installation, offline frozen
installation, package scripts under both aliases, and direct Edge resolution.
New artifacts are built and verified by the integration task:
[`dev_011_wasix_ci_release`](../../dev_011_wasix_ci_release/001_build_and_toolchain.md).
