# JavaScriptCore N-API On macOS

The bundled JavaScriptCore provider requires a JavaScriptCore runtime with `SharedArrayBuffer` and raw SharedArrayBuffer-backed typed-array byte access. The stock macOS `JavaScriptCore.framework` does not satisfy that requirement on this machine, so env creation is expected to fail unless the tests are run against a compatible external runtime.

## Default Layout

- Bun WebKit release tag: `autobuild-00e825523d549a556d75985f486e4954af6ab8c7`
- CI Bun WebKit SDK path: `<repo>/.ci/jsc/<tag>/macos-<arch>`
- Local Bun WebKit SDK path: `/Users/syrusakbary/Development/bun-webkit/<tag>`
- Override Bun SDK location with `BUN_WEBKIT_ROOT`
- Fallback WebKit checkout: `/Users/syrusakbary/Development/WebKit`
- Override source checkout location with `WEBKIT_ROOT`
- Default edgejs JSC build directory: `<repo>/build-napi-jsc`

## Scripts

- [`fetch_bun_webkit_macos.sh`](/Users/syrusakbary/Development/edgejs/napi/jsc/tools/fetch_bun_webkit_macos.sh)
  downloads and extracts the pinned Bun macOS WebKit SDK for the current machine architecture.
- [`build_webkit_macos.sh`](/Users/syrusakbary/Development/edgejs/napi/jsc/tools/build_webkit_macos.sh)
  clones or updates the Apple/macOS WebKit checkout and runs `Tools/Scripts/build-webkit --release`. This remains the source-build fallback.
- [`run_napi_jsc_tests_macos.sh`](/Users/syrusakbary/Development/edgejs/napi/jsc/tools/run_napi_jsc_tests_macos.sh)
  reuses `make build-napi-jsc`, auto-detects either the Bun static SDK or a local WebKit framework build, runs a direct JavaScriptCore runtime probe, then runs the JSC smoke suite and full `ctest` suite.

## Typical Flow

```bash
napi/jsc/tools/fetch_bun_webkit_macos.sh
make build-napi-jsc BUN_WEBKIT_ROOT="$(pwd)/.ci/jsc/autobuild-00e825523d549a556d75985f486e4954af6ab8c7/macos-<arch>"
napi/jsc/tools/run_napi_jsc_tests_macos.sh
```

The fast path uses the Bun SDK with:

- `NAPI_JSC_INCLUDE_DIR=$BUN_WEBKIT_ROOT/include`
- `NAPI_JSC_LIBRARY=$BUN_WEBKIT_ROOT/lib/libJavaScriptCore.a`
- `NAPI_JSC_EXTRA_LIBS=$BUN_WEBKIT_ROOT/lib/libWTF.a;$BUN_WEBKIT_ROOT/lib/libbmalloc.a;icucore`

No `DYLD_*` injection is needed in Bun mode because the JSC archive links statically into the test binaries.

The GitHub Actions `build-napi-jsc-macos` job uses this Bun SDK path only for N-API validation. It does not change the default `make build` provider or the release packaging path, which remain on bundled V8.

The source-build fallback still uses:

- `NAPI_JSC_INCLUDE_DIR=$WEBKIT_ROOT/WebKitBuild/Release/JavaScriptCore.framework/Headers`
- `NAPI_JSC_LIBRARY=$WEBKIT_ROOT/WebKitBuild/Release/JavaScriptCore.framework/Versions/A/JavaScriptCore`

and injects:

- `DYLD_FRAMEWORK_PATH`
- `DYLD_LIBRARY_PATH`
- `__XPC_DYLD_FRAMEWORK_PATH`
- `__XPC_DYLD_LIBRARY_PATH`

to the WebKit product directory before the runtime probe and `ctest` run.

## Validation Outputs

`run_napi_jsc_tests_macos.sh` writes:

- `<build-dir>/jsc-runtime-probe.txt`
- `<build-dir>/jsc-runtime-report.txt`

These files record the loaded JavaScriptCore image, the WebKit commit, the derived include and library paths, and the test commands used for the last successful local run.

## Stock Host Guard

The negative guard test in [`test_67_jsc_config_guard.cc`](/Users/syrusakbary/Development/edgejs/napi/jsc/tests/runners/test_67_jsc_config_guard.cc) remains valid and is expected to pass against the stock macOS framework while env-dependent tests fail.
