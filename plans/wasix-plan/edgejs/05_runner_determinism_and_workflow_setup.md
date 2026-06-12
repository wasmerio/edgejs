# Runner Determinism and Workflow Setup

Why this is a problem:

The test signal is meaningless if GitHub and local runs use different Wasmer
versions, different sysroots, different compiler backends, or broad host-path
mounts. WASIX behavior can change with libc and runtime versions.

Minimal manifestation:

```sh
make test-wasix-quickjs-only
```

If the runner uses a different runtime backend or stale sysroot, failures can
look like runtime regressions when they are build-environment drift.

Proposed solution:

- Install the intended Wasmer version explicitly.
- Install the intended `wasixcc` and sysroot.
- Force `wasmer run --llvm` for this lane.
- Mount only the directories required by the test package.
- Preserve `HOME=/tmp`, `NODE_TEST_DIR=/workspace/test`, SSL cert volume, and
  `/etc` package volume.

Sketch:

```sh
curl https://get.wasmer.io -sSfL | sh -s "v7.2.0-alpha.3"

wasmer run --llvm --net \
  --env HOME=/tmp \
  --env NODE_TEST_DIR=/workspace/test \
  --volume "$EDGEJS_ROOT:/workspace" \
  --volume "$EDGEJS_ROOT/quickjs-wasm/etc:/etc" \
  --volume "$EDGEJS_ROOT/ssl-certs:/usr/local/ssl" \
  --cwd /workspace \
  "$EDGEJS_ROOT/quickjs-wasm" -- "$test"
```

## Proposed Solution References

Proposed solution can be found in:

**Reference commits:**

```text
edgejs f87e6848 Added node test runner for wasix quickjs
edgejs 2baeceea Added node test runner wasix quickjs to gh workflow
edgejs 352c55f3 Set wasmer version to 7.2.0-alpha.3 for wasix tests
edgejs c63d7771 Install wasmer 7.2.0-alpha.3 using curl and shell script...
edgejs fbbb21f7 Updated sysroot=v2026-05-26.1 wasixcc=v0.4.3
edgejs 566e59a5 Force llvm runtime
edgejs 9aefbbe2 Isolate mapped host directories for wasix node test runner
```
