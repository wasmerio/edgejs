# EdgeJS integration validation

Status: active; dependency preparation. Owner: EdgeJS worker. Depends on engine/N-API ready signal.
Write ownership: EdgeJS runtime/tests/build changes if necessary and this note;
no N-API or QuickJS writes, no other plan edits. Audit CI/build dependencies first,
initialize non-NAPI submodules safely, then build native QuickJS and WASIX and run
runtime checks. Reproduce exact failures and inspect LLDB before semantic fixes.
Coordinator owns commits/gitlinks/pushes/PRs. Other workers may be active.

## Environment and validation scope

- Isolated worktree: `/private/tmp/quickjs-upstream-sync/edgejs`.
- macOS arm64; CMake/Ninja/Apple Clang available. Wasmer 7.4.2,
  wasixcc 0.4.4 (LLVM 21.1.2), Node 26.0.0, pnpm 11.2.2 installed.
- Initializing `test`, `ssl-certs`, `deps/libuv-wasix`,
  `deps/openssl-wasix`, and `wasmer-examples` only. The nested N-API/QuickJS
  worktrees must not be passed to `git submodule update`.
- Native build: `make build-edge-quickjs-cli CMAKE_BUILD_TYPE=Release JOBS=4`.
- Native runtime: `make test-quickjs-only TEST_JOBS=4`, Intl tests, then focused
  engine-sensitive smoke tests and framework/standalone verification where practical.
- WASIX rebuild must run from `quickjs-wasm/` as `./build.sh`, then package
  validation, safe-mode smoke tests, and relevant runtime checks.

## Existing CI coverage caveat

The baseline Makefile lists `test-quickjs-lang` in `.PHONY` but defines no recipe;
`QUICKJS_LANG_TESTS` is also undefined. The QuickJS workflow invokes this target,
so its language step currently executes no tests. This is pre-existing and not
evidence of successful language validation. Direct smoke coverage will be recorded
separately. No source changes have been made for this caveat.
