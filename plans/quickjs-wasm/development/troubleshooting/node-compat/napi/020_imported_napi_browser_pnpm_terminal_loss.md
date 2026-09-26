# Imported N-API browser pnpm terminal loss

| | | Remarks |
| --- | --- | --- |
| **Status** | ✅ | Resolved: pnpm install, synchronous child execution, and unmodified Next.js development startup pass in the browser. |
| **Severity** | High | After the failure, subsequent shell commands such as `ls` produce no terminal output. |

## Failure

In `wasmer-sh`, enter the bundled Next.js example and run `pnpm install`.
The install appears to make progress, but afterward `pnpm run dev` and even
`ls` produce no output.

The smaller `node-express` install smoke completes and remains usable. The
Next.js reproduction emits repeated WISP messages for already-closed streams,
then Chrome reports an uncaught WebAssembly `RuntimeError: unreachable`. The
shell transcript subsequently becomes unavailable, indicating that the
browser runtime failed rather than Bash merely retaining the foreground
terminal.

The clean main-based port also exposes a deterministic provider regression:
Bash and `fs.readFileSync()` read the correct lockfile bytes, while
`fs.promises.readFile()` initially returns an equal-length zero-filled Buffer.
This is not a filesystem-binding bug. A browser `ArrayBuffer` cannot represent
an arbitrary subrange of WebAssembly linear memory, so pooled Buffers require a
mirror. The JS-backed N-API provider must synchronize that mirror whenever
control crosses between native guest code and observable host JavaScript.

## 2026-08-14 Next.js WebContainer follow-up

The remaining Next.js startup failure was not caused by its SWC WebAssembly
fallback. Next asks the active package manager for its registry during that
fallback. pnpm 10 implements `config get registry` by invoking
`npm config get registry`, but the Edge package exported both `pnpm` and `npm`
through the same pnpm bundle. The result was an unbounded
`pnpm -> npm-as-pnpm -> npm-as-pnpm` process chain.

The Edge package now mounts the real npm 11.8.0 distribution pinned by the
Node test dependency and exposes its npm CLI as the `npm` command. No Next.js
or pnpm source is patched, and the WebContainer compatibility marker remains.

Removing the recursion exposed two independent runtime defects:

1. Edge's synchronous spawn runner used `uv_shutdown()` for a one-way child
   stdin pipe. WASIX could return `UV_EACCES` after the child had completed,
   incorrectly converting successful `execSync()` calls into spawn failures.
   The provider-neutral implementation now closes the stdin handle after its
   final write, which is the complete lifecycle for a unidirectional pipe.
2. A JSPI worker could report itself idle when one blocking job completed even
   if another suspended blocking job was still active. Worker busy state is now
   transition-based on an active-job count. The generic browser SDK retains a
   two-way default, while wasmer-sh explicitly advertises four-way guest
   parallelism because its supported synchronous process chain can be
   `node -> sh -> pnpm -> npm`.

These changes are in native Edge code, the Edge package manifest, and Wasmer
SDK scheduling. Edge files under `lib/` remain unchanged.

The focused browser smoke, including `execSync("pnpm config get registry")`,
passes. A clean Next 16.3.0 fixture using the ordinary script
`WATCHPACK_POLLING=true next dev --webpack -H 0.0.0.0 -p 3000` also passes:

```text
shell ready:   7.7 s
pnpm install: 10.8 s
Next ready:    2.0 s
preview:       7.8 s
total:        28.3 s
```

`NEXT_TEST_WASM_DIR` is not used by the development script.

The WebContainer module-resolution compatibility path is a WASIX build
capability. Native code no longer discovers that capability by repeatedly
reading the mutable `process.versions.webcontainer` property during every
dynamic import. WASIX builds define `EDGE_WEBCONTAINER_COMPAT`; the JavaScript
property remains the public capability marker, while native builds omit the
compatibility fallback entirely.

## 2026-08-14 Next.js production-build quiescence follow-up

The ordinary production sequence initially reported a successful `next build`
but produced no `.next/BUILD_ID`, so `next start` correctly rejected the
incomplete output. SWC loaded and transformed successfully. Instrumentation
localized the early exit to Next's output-file tracing phase, where
`@vercel/nft` repeatedly awaits filesystem callbacks.

The minimal runtime reproduction was independent of Next:

```js
const fs = require('fs');
let completed = 0;
(async () => {
  for (let i = 0; i < 50; i++) {
    await new Promise((resolve) => {
      fs.readlink('/workspace/next', () => {
        completed++;
        resolve();
      });
    });
  }
})();
process.on('beforeExit', () => console.log(completed));
```

The browser runtime exited after 18 callbacks. Every individual filesystem
operation and `process.nextTick` callback worked; the defect was Edge's outer
event-loop quiescence test. A provider checkpoint could drain one Edge task,
leaving the task queue empty at that instant. Edge then counted the turn as
idle even though the drained callback's Promise continuation would schedule
the next operation at the following host checkpoint. A chain longer than the
fixed idle grace window therefore exited cleanly but prematurely.

`CompleteProviderEventLoopCheckpoint` now reports two different facts:

- whether Edge work remains pending after the drain; and
- whether the checkpoint drained Edge work and therefore made progress.

The outer loop resets its idle decision on either pending work or progress and
continues until the combined provider/Edge queues reach a fixed point. This is
provider-neutral Node event-loop behavior: there is no `__wasi__` branch,
Next-specific keepalive, timer, or new N-API operation.

Verification with a rebuilt exnref host-N-API WebC used the fixture's ordinary
commands, without `NEXT_TEST_WASM_DIR` or an explicit SWC package. `pnpm build`
completed with status 0 and wrote `.next/BUILD_ID`; `pnpm start` reached ready,
opened port 3000, and rendered `Welcome to Next.js on Wasmer.` in the browser.
The source-map warning for the generated host-N-API function URL is separate
diagnostic noise and was not involved in the missing build.

## Buffer ownership action plan (completed)

1. Keep mirror coherence and mirror lifetime as separate mechanisms in the
   JS-backed N-API provider.
2. Copy host bytes into live guest mirrors before native callback entry and
   after returning from host JavaScript.
3. Copy guest bytes into host values before invoking host JavaScript, settling
   a Promise, yielding to the host event loop, and returning from a native
   callback.
4. Release only mappings whose native callback frame has ended and which have
   no retaining N-API reference.
5. Remove backend-specific copy triggers from Edge; keep `lib/` and
   `src/internal_binding/binding_fs.cc` unchanged.
6. Verify focused async filesystem and gzip/network cases before the full
   browser Next.js gate.

## Root causes and fix

The imported Edge runtime replaces globals on the shared worker realm. Panic
and worker-error reporting dynamically called `console.error`, while thread
parallelism dynamically read `navigator`. After Edge started, both lookups
could recursively enter N-API while the guest was already active. The SDK now
captures the host console and hardware concurrency before starting Edge.

Large installs also exposed excess copying in the native N-API buffer bridge.
Buffer, array-buffer, typed-array, and data-view lookups retained their host
snapshot and could request a second snapshot before copying into guest memory.
They now consume and free the existing snapshot. Allocations of at least 16 MiB
are no longer rounded up to the next power of two.

Finally, the Node and browser WISP network bridges could report socket `close`
before buffered data had been drained. Close is now deferred until the guest
reads the remaining bytes. The WISP receive allowance is 128 MiB for large
package archives; the associated memory-exhaustion concern is recorded in
`SECURITY-HOST-JS-NAPI.md`.

All fixes are in Wasmer SDK or native N-API code. Edge files under `lib/` and
`src/internal_binding/binding_fs.cc` remain unchanged.

## Verification

The optimized SDK was rebuilt against the required local Wasmer checkout:

```sh
cd js
WASMER_REPO=/Users/syrusakbary/Development/wasmer npm run build
```

The artifact is 4.35 MiB raw / 1.11 MiB Brotli. The SDK test suite passes, as
does this focused browser sequence:

```sh
cd wasmer-sh
WASMER_TEST_PNPM_ONLY=1 WASMER_PNPM_TIMEOUT=120000 npm test
```

That test now requires, in order: successful `pnpm install`, a returned Bash
prompt, visible output from a following `ls`, and output from a following Node
process. The fresh Next fixture also completed install and passed its
post-install `ls` assertion.

Next's development server did not reach its ready marker during the final
bounded diagnostic run. At the point the test was stopped, the renderer used
about 4.6 GiB RSS. This is now a separate performance/runtime compatibility
issue rather than terminal loss.

## Original action plan

1. Change the focused Next browser regression to run install separately from
   the development server.
2. Require the Bash prompt to return after install and verify a subsequent
   `ls` marker before attempting `pnpm run dev`.
3. Preserve browser `pageerror`, worker, and terminal diagnostics so the first
   Wasm trap has an actionable stack instead of surfacing only as a silent
   terminal.
4. Determine whether the trap is in Edge's imported N-API callback path, the
   Wasmer JSPI/yield boundary, or WISP socket completion handling.
5. Fix the native N-API or Wasmer side; do not modify files under EdgeJS
   `lib/` or `src/internal_binding/binding_fs.cc`.
6. Rebuild the N-API-backed Edge WebC and Wasmer SDK as needed, then validate
   install, prompt recovery, `ls`, and Next development-server startup.

## Initial verification

Registry-backed `syrusakbary/edgejs@0.1.8`:

```sh
cd wasmer-sh
WASMER_TEST_PNPM_ONLY=1 WASMER_PNPM_TIMEOUT=240000 npm test
```

passes for `node-express`.

The existing combined Next command:

```sh
WASMER_NEXT_TIMEOUT=600000 node tests/next-browser.mjs
```

reproduces the WebAssembly `unreachable` and terminal loss before the Next
ready marker.

## 2026-09-26: consistent public npm alias

The user wants both public package-manager commands to use pnpm. The standard
package still ran real npm while QuickJS aliased npm to pnpm and retained the
recursive fallback risk. Plan: expose real npm as `edge-npm-internal`, set
`npm_config_npm_path=/bin/edge-npm-internal` for both public commands, and keep
Pi, pnpm and Edge runtime sources unchanged. QuickJS must include the same
pinned npm assets. Check both aliases' versions, a custom registry lookup
through npm delegation, installation and script execution in both packages
before publication; keep the browser demo on the existing URL.

Implemented through package manifests only. Both aliases have identical pnpm
arguments and environment within each package, including a default command
`PATH` so package scripts can find `node` in a direct Wasmer CLI invocation.
The shared distribution target now includes all mounted npm/pnpm assets and
chooses the QuickJS manifest itself, replacing the workflow's incomplete
second ZIP assembly. The QuickJS WASIX CI job runs the package-manager smoke
test.

Validation: the enhanced `scripts/test-wasix-pnpm.py` passes with the native
Wasmer CLI and embedded QuickJS. Both variants also pass through the Node SDK:
both public versions are 10.34.5; a project `.npmrc` registry is returned through
the real-npm fallback; `npm install react@19.2.8` creates the pnpm lockfile;
`pnpm install --offline --frozen-lockfile` reuses it; both aliases execute the
installed package script. Both distribution directories pass
`wasmer package build --check`.

A separate Node SDK diagnostic using `spawnSync` to capture nested registry
lookups returned status 0 with empty output. This reproduces with the published
0.2.2 imported-N-API package, including its original real-npm command, as well
as both new packages. Direct SDK command output and native CLI tests pass.
This existing nested stdio issue is not fixed by the packaging change.
