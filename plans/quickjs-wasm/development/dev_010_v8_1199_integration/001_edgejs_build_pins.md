# V8 11.9.9 integration

| | | Remarks |
| --- | --- | --- |
| **Status** | 🟠 | Integrated and validated; the broader CTest suite retains a pre-existing intermittent REPL failure. |
| **Date** | 2026-09-07 | EdgeJS portion of the coordinated N-API, Wasmer, and SDK update. |

## Scope and ownership

The EdgeJS worker owns the root build pins, the `napi` submodule pointer, and
these build notes. A separate integration worker owns N-API implementation
changes, Wasmer SDK branch updates, and the SDK companion PR. Their write sets
are disjoint. The EdgeJS commit depends on the validated N-API commit supplied
by that worker; remote integration follows the local EdgeJS validation.

Update `Makefile` and the Linux amd64 Nix development shell to custom build
`11.9.9`, then advance `napi` to the same build and linker fix. Preserve existing
runtime behavior and historical verification records. Current build facts live
in [the V8 backend notes](../../../napi-v8/README.md).

## Verification expectations

- Verify the downloaded Linux amd64 release archive against GitHub's SHA-256
  digest before recording its SRI hash in `flake.nix`.
- Build native V8-backed EdgeJS against the actual Darwin arm64 `11.9.9`
  release and run Edge runtime smoke and compatibility tests.
- Leave N-API implementation suites to the N-API worker rather than duplicating
  them in EdgeJS.
- Record results and any environment limitations before committing.

## Integration facts

- The `napi` submodule is pinned to
  `38834f059fea9df3585e2d76d31c255fa13f5dc9`, based on N-API `main` and carrying
  the `11.9.9` defaults plus platform-correct native linker dependencies.
- The downloaded Linux amd64 archive matched GitHub's SHA-256 digest:
  `32d15e5efd1dd19afd48174b1908f7d8813c8433fbfbfe616c06f380319a716d`.
  Its Nix SRI hash is
  `sha256-MtFeXv0d0Zr9SBdLGQj32IE8hDP7+/5hbAbzgDGacW0=`.
- The Darwin arm64 release used for the native build also matched GitHub's
  digest: `5f182225758afa840acb463ecaf1662414ce735c1ec626e78cd2373586d591c7`.
- Nix is not installed on the validation host, so the archive/hash check does
  not claim a Nix development-shell evaluation.

## Verification results

- Native `make build JOBS=4` passed against the verified Darwin arm64 `11.9.9`
  archive, including a final reconfigure/rebuild with the pinned N-API commit.
- The shipping CLI passed the runtime/zlib smoke test and reported engine
  version `13.6.233.17-node.0`, consistent with the release headers.
- `make test-only TEST_JOBS=4`: **1749/1749 passed**.
- Edge-owned CTest: **1441 passed, 18 expected skips, 1 failure** out of 1460.
  The failure was the embedded REPL multiline-navigation test's child process
  exiting with SIGSEGV. A direct shipping-CLI run of that fixture passed 30/30.
- A controlled baseline comparison reproduced the intermittent CTest failure
  with the old release too: **3/30 failures with 11.9.2**, versus **2/30 with
  11.9.9**. Both test and subprocess CLI binaries were linked from the same
  objects against their respective verified release archive. Native N-API C++
  sources are unchanged between the old and new submodule pins, and all used
  V8 headers are byte-identical; only the unused `wasm-c-api/wasm.h` differs.
  This pre-existing harness-path failure is not fixed or hidden by this pin
  update.
- `git diff --check` passed. No Edge runtime sources changed, and no duplicate
  N-API implementation suites were added or run in EdgeJS.
