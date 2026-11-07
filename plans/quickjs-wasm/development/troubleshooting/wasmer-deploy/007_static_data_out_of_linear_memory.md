# WASIX static data out of guest linear memory

| | | Remarks |
| --- | --- | --- |
| **Status** | 🟠 | Implemented and verified locally on `fix/wasix-static-data-files`; not merged or released. Caveats under Verification. |
| **Severity** | Medium | Long-running SDK hosts reach 1.7-2.6 GB (wasmer-sdk#547); Edge.js runs, but every instance carries ~45 MB of private static data. |

## Request and context

On 2026-11-07 the user asked to fix wasmerio/wasmer-sdk#547 ("Closed sandboxes
running wasmer/edgejs keep ~50 MB each") at the root cause, and asked whether
Edge.js should "do something similar to Node". This note covers the Edge.js
half. The SDK half (releasing dead guest memories deterministically) is tracked
in the SDK repository.

## Findings

Measured on macOS arm64, Node 26, @wasmer/sdk main, `wasmer/edgejs` 0.2.5, with
the issue's loop (create sandbox, `edge /workspace/main.js`, close; 60 times):

- Each Edge.js run creates one shared guest `WebAssembly.Memory` of 826 pages
  (51.6 MB). In the SDK it outlives the run until the pool worker's V8 heap
  collects it, which idle workers rarely do. The in-loop plateau is about
  1.1-1.3 GB on macOS.
- 45.6 MB of that initial memory is three passive data segments copied into
  every instance by `memory.init`:
  - about 33.1 MB of full ICU 78 common data (`icudt78l`), embedded by
    `cmake/embed_binary.py` from `deps/icu-small/source/data/in/icudt78l.dat.bz2`
    (`wasix/cmake/icu_wasix.cmake`) and registered with `udata_setCommonData`
    (`src/edge_icu_data.cc`);
  - about 10.1 MB of builtin JS (Node `lib/` plus `deps/` JS), embedded by
    `scripts/generate_builtin_catalog_header.py` (`cmake/EdgeBuiltinCatalog.cmake`)
    and read in `src/builtin_catalog.cc`.
- Native Node keeps the same data in its read-only executable image (`__TEXT`,
  clean, file-backed, shared between processes). Wasm has no read-only segment, so
  each WASIX instance gets a private dirty copy.
- The same bytes are also duplicated outside the guest, because they live inside
  `edge.wasm` (56.1 MB): in the webc, in the SDK's module cache, and in V8's
  compiled-module wire bytes.

A prototype (scratch build, not merged) that reads builtins from `/node-lib` and
ICU items lazily from `/usr/share/edge-icu` measured:

- guest memory 51.6 -> 10.4 MiB at start, 52 -> 11 MB at exit;
- `edge.wasm` 56.1 -> 12.9 MB;
- in-loop plateau 1166 -> 948 MB and process floor -84 MB; latency neutral.

## Action plan

1. Add CMake options `EDGE_EMBED_BUILTIN_SOURCES` and `EDGE_EMBED_ICU_DATA`
   (default ON, so native builds are unchanged). Both WASIX build scripts
   (`wasix/build-wasix.sh`, `quickjs-wasm/build.sh`) set them OFF.
2. Builtin catalog without sources: keep ids, paths and categories so
   `builtinModules`, `require.resolve` and `canBeRequired` are unchanged; read
   sources from the lib/deps roots (`/node-lib`, `/node-lib/internal/deps`, or
   `EDGEJS_LIB_DIR`). Fail fast with a clear error at bootstrap when the lib root
   is missing.
3. ICU in individual-file mode: when not embedded, call `u_setDataDirectory`
   (`EDGE_ICU_DATA`, then `ICU_DATA`, then `/usr/share/edge-icu`) and report a
   clear error when the data directory is missing.
4. Packaging: stage exactly the catalog's JS files into `node-lib/` and the ICU
   items into `icu/` in `dist-only`, and declare `"/node-lib"` and
   `"/usr/share/edge-icu"` in both package manifests' `[fs]` sections.
5. Test lanes and CI that run the raw `edgejs.wasm` must mount the same data
   (or keep using an embedded build). Exact list: see the integration map below.
6. Tests: a size guard (data section and initial memory pages for both WASIX
   builds), an Intl/encoding correctness script under both WASIX variants, a
   missing-mount error test, and the existing `make test-quickjs-only` /
   `make test-only` native lanes unchanged.
7. Measure with the SDK issue loop and record the numbers here.

## Integration map

A read-only review of the tree (four mappers plus a completeness critic) found
these integration points. Line numbers refer to `main` at `eb6573bb`.

- **Catalog generator.** In `cmake/EdgeBuiltinCatalog.cmake`, a bare
  `$<$<NOT:...>:--no-sources>` generator expression under `VERBATIM` passes an
  empty argument when the option is ON, which breaks native builds. It needs
  `COMMAND_EXPAND_LISTS`. Reproduced with CMake 4.3.2.
- **Lib root in file mode.** The lib root must come only from `EDGEJS_LIB_DIR`
  or `/node-lib`. The existing fallbacks (cwd `lib/`, source root,
  exec-relative) would otherwise load a user's own files as builtins. A
  catalog digest written next to the staged tree catches a binary paired with
  a stale tree.
- **ICU file access.** ICU opens items through its POSIX mmap path on WASIX
  (`umapfile.h`); `U_HAVE_MMAP=0` would switch it to stdio. The per-item
  memory cost has not been measured yet.
- **Build scripts.** `wasix/build-wasix.sh` deletes its CMake cache on every
  run, while `quickjs-wasm/build.sh` keeps it unless
  `WASIX_FORCE_RECONFIGURE=1`, so the options must be passed explicitly on
  every run. `wasix/validate-imported-napi-wasm.py` already reads the cache and
  is the place for the size guard.
- **Test lanes.**
  - The V8 lanes run the raw wasm through `napi_wasmer`, which takes
    `--builtin-js-dir` (default: the repo's `lib/`) and fails loudly on a
    missing directory.
  - The wasmer branch of `scripts/edge-wasix-node-runner.sh` forwards only
    the first four `--env` specs.
  - Several Makefile targets depend only on the wasm, not on staged data.
- **Packaging.**
  - `wasmer.toml` already carries commented-out `/node-lib` lines, and
    `quickjs-wasm/wasmer.toml` has no mount.
  - `Makefile` `dist-only` rewrites manifest sources with perl and zips a
    fixed file list.
  - `scripts/ci/release.py` rejects escaping sources and symlinks.
- **Volume vs `[fs]` (measured, Wasmer 7.5.0).** A package `[fs]` entry wins
  over `wasmer run --volume` at the same guest path; the hidden `--mapdir`
  flag does replace it. The wasmer runners therefore rely on the package
  `[fs]` for `/node-lib` and `/usr/share/edge-icu`, and mount overrides
  (`WASIX_EDGEJS_NODE_LIB`, `WASIX_EDGEJS_ICU_DATA`) under
  `/edgejs-static-data-override`, selected with `EDGEJS_LIB_DIR` /
  `EDGE_ICU_DATA`. Dotfiles such as `node-lib/.edge-builtin-catalog` survive
  both `wasmer run <dir>` and `wasmer package build` (Wasmer 7.5.0).
- **Not yet known.** The per-run cost of ~5k extra package files in
  `wasmer run <dir>` test lanes; registry and webc limits on file counts.
  QuickJS WASIX with external ICU had never been run.

The full checklist with file:line references and a test per item was
generated during the review (local scratch, not committed).

## Verification

### Builds (2026-11-07, macOS arm64, wasixcc sysroot from the build scripts)

| Build | Data section | Initial memory | `edgejs.wasm` |
| --- | --- | --- | --- |
| host N-API, embedded (before) | 45,628,965 B | 826 pages (51.6 MB) | 56.1 MB |
| host N-API, files (after) | 2,449,573 B | 167 pages (10.4 MB) | 23.0 MB before `wasm-opt` stripping |
| QuickJS, embedded (before) | 45,766,093 B | 828 pages (51.8 MB) | 63.3 MB |
| QuickJS, files (after) | 2,586,765 B | 169 pages (10.6 MB) | 20.1 MB |

Both builds staged 723 builtin JS files plus the catalog digest into
`<build>/node-lib` and 4305 ICU items into `<build>/icu/icudt78l`, and passed the
new size guard (data section under 4 MiB, at most 200 initial pages).

- **Stale catalog header (fixed).** The first host N-API rebuild failed the
  size guard with a 12.5 MB data section: a header generated earlier in
  embedded mode was newer than all its dependencies, and CMake's Makefile
  generator does not rerun a custom command when only its arguments change.
  `cmake/EdgeBuiltinCatalog.cmake` now writes a mode stamp with
  `file(CONFIGURE)` and lists it as a dependency; a toy project confirmed that
  ON -> OFF -> ON -> OFF flips regenerate the output every time.
- **First QuickJS run with file-backed ICU.** `wasmer run quickjs-wasm` printed
  `Januar あ aäz 78.2 17.0` for a German month name, a Shift_JIS decode, a
  German collation and `process.versions.icu/unicode`, with nothing on stderr.

### First verification round

Seven parallel checks against the new builds, with failures classified against
embedded builds of the same commit (`build-wasix-embedded/`,
`build-quickjs-wasix-embedded/`):

- **V8 lanes:** no regressions. `test-wasix-v8-only` loses 2-6 loopback
  HTTP/HTTP2 tests per full run, a different set each time, and they pass
  alone on both builds. The embedded baseline loses 4 the same way. Every
  other host N-API lane passes.
- **QuickJS lanes:** no regressions. Intl 6/6, process 1/1, safe mode 12/12,
  frameworks 13/0/3, standalone 3/0/1. Per-test time is unchanged, and
  `wasmer run <dir>` startup is 60-75 ms faster (less passive data to
  initialise).
- **Failure and override paths:** each behaves as designed. A missing ICU
  directory gives one warning per process. A missing or stale `/node-lib` is a
  hard error, but only a warning under `EDGEJS_LIB_DIR`. A deleted builtin
  names its path. A fake `lib/fs.js` in the working directory is never loaded.
  `process.binding('natives')` returns the same 720 sources in every build.
- **Packaging:** both dists pass `wasmer package build --check` and the
  `release.py` checks. The webc grows by 0.8% (+0.6 MB), with 7283 files.
- **Native:** the catalog header differs only by the two new constants.
  `make test-quickjs-only` and `make test-only` each fail only
  `parallel/test-dns-perf_hooks` (this machine's DNS takes 91-99 s, the same on
  `main`).

Two problems were found and fixed:

- **ICU file mapping cost.** ICU mapped every data item with `mmap`. WASIX's
  emulated `mmap` rounds each mapping up to 64 KiB of linear memory, and ICU
  keeps them for the life of the process. An Intl-heavy QuickJS sweep opened
  855 items (15 MB of data, 57 MB once rounded) and ended 16.2 MB above the
  embedded build. `wasix/cmake/icu_wasix.cmake` now compiles ICU with
  `U_HAVE_MMAP=0`, so ICU reads each item into a buffer of its real size.
- **ICU directory overrides.** A set but unusable `NODE_ICU_DATA` or
  `--icu-data-dir` disabled ICU entirely. The embedded build ignored these
  settings. The precedence is now `--icu-data-dir`, `EDGE_ICU_DATA`,
  `NODE_ICU_DATA`, `ICU_DATA`, then `/usr/share/edge-icu`. A directory counts
  if it holds `icudt78l/` or a Node-style `icudt78l.dat`, and an unusable
  explicit setting warns once and falls back to the package mount.

The SDK loop on the first round confirmed the per-instance cost: guest memory at
exit is 11.1 MB instead of 52.3 MB on host N-API. Peak RSS for QuickJS in a
tight loop went up, because smaller dead memories put even less pressure on the
SDK workers' GC. That retention is fixed on the SDK side (wasmer-sdk#547), not
in Edge.js.

### Second verification round

Four parallel checks after the two fixes:

- **ICU memory.** In the SDK, an Intl-heavy QuickJS sweep now ends at 52.4 MB
  of guest memory (mmap build 94.8 MB, embedded 78.6 MB). Its extra growth over
  the embedded build is 15.0 MB, the size of the 855 items it read. ICU reads
  them with `fopen`/`fread` (confirmed with `llvm-nm` on `umapfile.cpp.obj`).
  The sweep is about 115 ms slower than the embedded build in the guest
  (288 ms vs 172 ms): the cost of reading the files.
- **Issue loop (host N-API, N=60).** Guest memory 11.1 MB at exit instead of
  52.3 MB; RSS plateau 738 MB instead of 899 MB.
- **Lanes.** Both intl lanes, safe mode, the QuickJS framework lane and a
  serial QuickJS lane (1675 pass / 0 fail) pass. The parallel `-only` lanes
  lose only loopback HTTP tests that pass alone on both builds.
- **ICU directory overrides.** 35 of 36 targeted cases behaved as specified.
  The exception (`--icu-data-dir <dir>` with a space) fails the same way on
  native and embedded builds.

The review then asked for three more ICU directory changes, made and rebuilt:

- a directory now counts only if its files carry ICU data headers
  (`res_index.res` and `root.res`, or `icudt78l.dat`), so an empty or foreign
  file warns and falls back instead of silently disabling Intl;
- `:`-separated path lists are accepted, as ICU itself accepts them;
- clearer messages, for example `ignoring ICU data directory /nope (from
  ICU_DATA): it holds no usable icudt78l/ or icudt78l.dat; using
  /usr/share/edge-icu instead`.

All 36 recorded override cases were replayed on the final build. 30 are
byte-identical, 3 differ only in worker output order, and the 3 corrupt-data
cases now warn and fall back as intended. Path lists, the unit tests (validator
18/18, release 18/18, safe mode 7/7), safe mode on the QuickJS package (14/14)
and both intl lanes pass on the final build.

### Known caveats

- **QuickJS loopback flakes under parallel load.** With `TEST_JOBS=8`, the
  file-backed QuickJS build loses more loopback-connect tests
  (`ECONNREFUSED`) than the embedded build: 15 vs 1 over six runs. Every one
  passes alone on both builds, and the serial lane passes 1675/0. Not
  investigated further.
- **Intl-heavy QuickJS startup.** First use of many locales reads hundreds of
  small files (about +115 ms for a 104-locale sweep). Programs that touch one
  locale are unaffected.
- **CI Wasmer versions.** `[fs]` precedence over `--volume` and the packaged
  dotfile were checked on Wasmer 7.5.0 only; CI uses v7.2.0-alpha.3 (nightly)
  and v7.4.2 (release). The new CI smoke steps are the check there.
- **Package layout.** The package gains 5029 files (723 builtins, 4305 ICU
  items, the digest). The webc grows by 0.8%.
- **Still open from the review (minor):** benchmark scripts do not mount ICU;
  `tests/js/wasix-icu-data.js` has no string-width check; the safe-mode suite
  needs `--embedded-static-data` for embedded packages; no unit tests for the
  two new Python scripts.

## References

- wasmerio/wasmer-sdk#547.
- Investigation and design reports (local scratch, not committed):
  `i547/synthesis.md` and `i547d/recommendation.md`; prototype patch
  `i547d/P3/p3-edge-static-data.patch`.
