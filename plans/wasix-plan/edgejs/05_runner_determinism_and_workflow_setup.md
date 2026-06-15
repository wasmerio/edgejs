# EdgeJS: runner determinism and workflow setup

Why this is a problem:

The WASIX QuickJS Node suite is only useful if local and GitHub runs use the
same runtime backend, Wasmer version, sysroot, package mounts, and test root
rewrites. If one side silently uses a different sysroot or compiler backend, the
CSV starts mixing product regressions with harness drift.

This occurs in full-suite comparison runs, focused retries, and GitHub workflow
runs for `make test-wasix-quickjs-only`.

Why native EdgeJS passes, and why WASIX still needs this:

Native EdgeJS tests run directly against one host binary and one host OS. There
is no Wasmer compiler backend, sysroot tag, package mount list, or guest/host
path rewrite in the middle. The WASIX test path has all of those extra moving
parts, so a test can fail because CI used a different runtime, an older libc, a
missing mount, or a host path that is meaningless inside the guest. The EdgeJS
change is justified because the runner is EdgeJS test infrastructure: it must
make the WASIX environment deterministic enough that remaining failures can be
attributed to EdgeJS, libuv, `wasix-libc`, or Wasmer behavior rather than harness
drift.

Minimal Example:

```sh
WASMER_BIN=~/src/wasmer/target/release/wasmer \
  make test-wasix-quickjs-only

python3 test/tools/test.py \
  --timeout 2 \
  --test-root test \
  --shell ./scripts/edge-wasix-node-runner.sh \
  parallel/test-dns
```

Failing-test evidence came from whole-suite columns where the same test changed
status only because the runner/sysroot/backend differed, especially DNS, UDP,
TLS, pseudo-tty, and stack-sensitive rows.

Callgraph and boundary:

Current problematic path:

```text
make test-wasix-quickjs-only
  -> nodejs_test_harness
  -> scripts/edge-wasix-node-runner.sh
  -> wasmer run quickjs-wasm
     HERE IS THE PROBLEM: backend, sysroot, Wasmer version, mounts, or host-path
     rewrites can differ between local and CI.

GitHub workflow
  -> installs downloaded wasmer/wasixcc/sysroot
     HERE IS THE PROBLEM: pre-release runtime and libc changes are hidden unless
     the workflow pins exactly the intended versions or sources.
```

The boundary is test harness/package configuration, not the JavaScript API. A
bad runner makes every area look suspicious.

Proposed solution:

Make the runner deterministic and visible:

- force `wasmer run --llvm`;
- pin/install the intended Wasmer pre-release in CI;
- build with the intended `wasix-libc` sysroot;
- mount only the directories needed by the guest;
- rewrite host test paths and reporter paths to guest paths;
- print the effective `wasmer run` command for debugging.

Relevant EdgeJS code paths:

```text
~/src/edgejs/Makefile
~/src/edgejs/scripts/edge-wasix-node-runner.sh
~/src/edgejs/quickjs-wasm/build.sh
~/src/edgejs/quickjs-wasm/wasmer.toml
~/src/edgejs/.github/workflows/test-and-build-quickjs.yml
```

Proposed callgraph:

```text
make test-wasix-quickjs-only
  -> harness passes test path + reporter path
  -> runner rewrites host paths to /workspace paths
  -> runner invokes pinned WASMER_BIN with --llvm
  -> quickjs-wasm package sees stable /workspace, /etc, /usr/local/ssl, HOME
  -> CSV column reflects product behavior, not harness drift
```

This is an EdgeJS-owned test-infra plan. It should not change runtime semantics.

## Proposed Solution References

### [wasmerio/edgejs#91: [WIP] Node tests using Edgejs WASIX QuickJS](https://github.com/wasmerio/edgejs/pull/91)

- Sadhbh: edgejs [f87e6848](https://github.com/wasmerio/edgejs/commit/f87e68487996f12d447974d0cefb85c3e956e885) Added note test runner for wasix quickjs
- Sadhbh: edgejs [2baeceea](https://github.com/wasmerio/edgejs/commit/2baeceeaba641eb8e5ad61a88c1caf5b75a24547) Added node test runner wasix quickjs to gh workflow
- Sadhbh: edgejs [352c55f3](https://github.com/wasmerio/edgejs/commit/352c55f3cf2d2b1083c48eb4cf190b3b43859ce0) Set wasmer version to 7.2.0-alpha.3 for wasix tests
- Sadhbh: edgejs [c63d7771](https://github.com/wasmerio/edgejs/commit/c63d77715b7f0d96ac1ef027cfa885ffcae229e2) Install wasmer 7.2.0-alpha.3 using curl and shell script. Disable (temporarily other workflows).
- Sadhbh: edgejs [fbbb21f7](https://github.com/wasmerio/edgejs/commit/fbbb21f7500bfa0f80cd84a49e63e579c929bebe) Updated sysroot=v2026-05-26.1 wasixcc=v0.4.3
- Sadhbh: edgejs [566e59a5](https://github.com/wasmerio/edgejs/commit/566e59a5b5dc451c4970dcc095ed80d6ed9e3120) Force llvm runtime
- Sadhbh: edgejs [9aefbbe2](https://github.com/wasmerio/edgejs/commit/9aefbbe29753d32a43dcd3b11b9cd8dbbcb99b30) Isolate mapped host directories for wasix node test runner
