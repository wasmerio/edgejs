# Build And Toolchain

Status: 🟢 Both WASIX builds and focused runtime checks passed (2026-10-02); full GitHub CI is pending.

## Scope

Synchronize main/submodules; build imported N-API and embedded QuickJS WASIX with wasixcc 0.4.7 and libc v2026-10-02.1. Preserve user edits.

## Dependencies and write ownership

Root agent owns toolchain pins, build directories, AGENTS.md, registries, and integration. Builds sequentially share OpenSSL outputs. Other workers may be active in the shared checkout.

## Verification expectations

Run import checks, package checks, safe-mode/process smoke tests for both artifacts.

## Context

User requests latest main including submodules, both WASIX builds with latest wasixcc, CI fix, and release-please support. Baseline main: 9bff8bc6. Installed Wasmer: 7.4.2. Main package/version header is 0.0.0; registry manifests differ. Do not publish a real release as part of implementation.

## Integration progress

- Fast-forwarded main from `93b9b830` to `9bff8bc6` and synchronized/updated recursive submodules. The N-API gitlink matches current upstream main (`a65e9cb8`); libuv now pins `6fe62598` with the parent pipe-end fix. QuickJS test262 remains intentionally excluded by its submodule update policy.
- Working branch: `codex/wasix-ci-release-please`. User changes to the QuickJS package version, generated test caches, `all-test/`, `deps/uvwasi/`, and the untracked WebAssembly threads fixture are preserved.
- Installed and latest compiler: wasixcc **0.4.7**, custom LLVM **21.1.2**. Installed exception sysroot matched extracted release `v2026-10-02.1` byte-for-byte; archive SHA-256 `c6fe6b4fd3e9576ed760c83a35f8d38e3d5f29e1b4d2df1af9d065d23278021a`. Both CI toolchain actions now use `v0.4.7` and the same sysroot tag.
- Removed only the ignored OpenSSL ABI stamp to force rebuilding the shared static dependencies with the new compiler; imported guest rebuild passed with **110 N-API imports, 67 extension imports**. Build log: `/private/tmp/edgejs-build-wasix.log`.
- Imported package loading/version, safe-mode smoke (including HTTP fetch, HTTPS get, and verified TLS connection), and `wasix-stdio-parent-close` all pass under Wasmer **7.4.2**. Logs: `/private/tmp/edgejs-imports-smoke.log`, `/private/tmp/edgejs-imports-network-smoke.log`, `/private/tmp/edgejs-imports-process.log`.
- First smoke invocation shared cold Wasmer caches with another invocation and produced a cache-removal warning rejected by the strict stderr checker; sequential rerun passed. The first manual process command omitted the imported guest exec-path override; the correctly configured rerun passed. These were verification-command issues, not runtime fixes.
- Cleaned the existing QuickJS build target and started `cd quickjs-wasm && WASIX_FORCE_RECONFIGURE=1 ./build.sh`; log `/private/tmp/edgejs-build-quickjs-wasix.log`.
- Stable releases use `EDGE_RELEASE_BUILD=1`; CMake suppresses the Git suffix and Makefile uses the exact package version. Development package references now include `g` before the short hash, matching the runtime semver convention. Existing `IS_FINAL_RELEASE` remains supported.
- All workflows pass `actionlint`. Release helper boundary tests pass with Homebrew Python **3.14** (9 tests); default macOS Python 3.9 lacks `tomllib`, while CI uses modern Python.

## Final local verification

- QuickJS clean rebuild completed. Its no-N-API-imports/non-WASI-import gate passed; build produced `build-quickjs-wasix/edge.wasm` and `edgejs.wasm`.
- QuickJS `wasmer package build --check`, runtime version check, safe-mode/network smoke, `make test-wasix-quickjs-process`, and the complete `make test-wasix-pnpm` passed against the rebuilt artifact. Logs: `/private/tmp/edgejs-quickjs-version.log`, `edgejs-quickjs-smoke.log`, `edgejs-quickjs-process.log`, `edgejs-quickjs-pnpm.log`.
- Final guest hashes: imported N-API `609dee8eef5c2b58eb837a3ebb3e60700a365a65af8c98f38d39642efb0812c7`; embedded QuickJS `1045abf5d87f079f27abd16c1d6c4be4b1c18e0612095f1c2743195ff71f76ab`. Both dev builds report `0.2.5-g9bff8bc`.
- Release workflow integration now requires successful validation for manual recovery too. PR validation uses a separate per-PR concurrency group, while automatic/manual publication remains serialized under `edgejs-release`.
- No live release tag, registry publication, or merge was performed. Full native/Linux CI is run through the draft PR; local checks are not a claim that all six CI lanes have passed.
