# Node compatibility test failure analysis

| | | Remarks |
| --- | --- | --- |
| **Status** | ▶️ | Historical failure clustering; individual failures have node-test issue pages. |
| **Severity** | High | Node compatibility failures remain significant until the linked issue pages close. |

Date: 2026-05-07
Last updated: 2026-05-15

This note is chronological. The May 7 sections describe the original
V8/default-run failure clustering. Current QuickJS issue status lives in the
later dated sections and the canonical
[`troubleshooting/node-test`](troubleshooting/node-test/README.md) registry.
EdgeJS root verification now focuses on Edge runtime tests; native N-API test
suites are owned by the N-API repository and should not be treated as EdgeJS CI
or root Makefile gates.

Command under investigation:

```sh
make test-only TEST_JOBS=4
NODE_TEST_RUNNER=build-edge/edge ./test/nodejs_test_harness --category=node:buffer,node:console,node:dgram,node:diagnostics_channel,node:dns,node:events,node:http,node:https,node:os,node:path,node:punycode,node:querystring,node:stream,node:string_decoder,node:tty,node:url,node:zlib,node:crypto,node:domain,node:http2,node:tls,node:sys \
  --skip-tests=known_issues/test-stdin-is-always-net.socket.js,parallel/test-dns-perf_hooks.js,parallel/test-dns-channel-timeout.js
```

The pasted CI run ends with 92 failures out of 1685 tests. A local rerun from
this workspace also exits with code 2, but the local failure count is larger
because the current workspace build/environment exposes the same broken surfaces
across more tests. The root-cause grouping below is based on the pasted CI
signatures and source inspection.

## Summary

Most failures are not independent subsystem regressions. They cluster around a
small number of incomplete Node compatibility surfaces:

1. `require('v8')` can crash during module evaluation because
   `internalBinding('config').hasInspector` is advertised as true while the
   `profiler` internal binding is not implemented.
2. The global `console` does not write through to stdio in normal `console.*`
   calls, which also breaks proxy fixtures that log status/body with
   `console.log()`.
3. The CLI `-p` / `--print` path evaluates code but does not print expression
   results.
4. JS-transferable structured clone support loses `File` identity and returns a
   plain object.
5. Some flags are accepted or exposed even though the backing feature is missing
   or incomplete: inspector, CPU/heap profiling, HTTP/HTTP2 debug warning
   shape, and string-decoder maximum string checks.

## Root Cause 1: `node:v8` Profiler Crash

Representative failures:

- `test-dgram-async-dispose`
- `test-dgram-*-cluster-*`
- `test-diagnostics-channel-process`
- `test-events-add-abort-listener`
- `test-http-server-drop-connections-in-cluster`
- `test-domain-top-level-error-handler-throw`
- `test-domain-uncaught-exception`
- `test-http2-ping-settings-heapdump`
- several TLS tests using `node:test`, `child_process.fork()`, or coverage/test
  runner helpers

Observed signature:

```text
node:v8:508
  takeCoverage: profiler.takeCoverage,
                       ^
TypeError: Cannot read properties of undefined (reading 'takeCoverage')
```

Why it happens:

- `lib/v8.js` initializes `profiler` from `internalBinding('profiler')` whenever
  `internalBinding('config').hasInspector` is true.
- `src/internal_binding/binding_config.cc` currently hard-codes
  `hasInspector = true`.
- `src/internal_binding/dispatch.cc` has no `profiler` resolver, so unresolved
  bindings return undefined.
- `lib/v8.js` then exports `takeCoverage: profiler.takeCoverage`, which throws
  if `profiler` is undefined.

Relevant code:

- `lib/v8.js`: profiler initialization at lines 54-57, export dereference at
  lines 508-509.
- `src/internal_binding/binding_config.cc`: `hasInspector` is set true at lines
  87-113.
- `src/internal_binding/dispatch.cc`: resolver list has no `profiler` entry and
  unknown bindings return undefined at lines 211-274 and 338-344.

This is a fan-out bug. Fixing this one surface should remove many unrelated
looking dgram, cluster, diagnostics, events, HTTP, HTTP2, TLS, domain, URL, and
zlib failures that only happen because those tests load `node:v8` indirectly.

## Root Cause 2: Global Console Does Not Write

Representative failures:

- `test-console-clear`
- `test-console-count`
- `test-console-diagnostics-channels`
- `test-console-methods`
- `test-console-stdio-setters`
- `test-console`
- pseudo-TTY console color tests
- HTTP/HTTPS proxy tests whose fixtures use `console.log()`

Observed signatures:

- `console.count()` leaves captured stdout empty instead of writing
  `default: 1\n`.
- `console.clear()` writes nothing instead of cursor-control escape sequences.
- `new console.log()` does not throw the expected TypeError.
- Proxy fetch fixture stdout is empty even though the child exits with code 0.
- Proxy request fixtures often contain only the raw response body because
  `res.pipe(process.stdout)` works, while status lines emitted with
  `console.log()` are missing.

Why it happens:

- `lib/internal/console/global.js` creates the global console from
  `internal/console/constructor` and lazily binds `_stdout` / `_stderr`.
- `lib/internal/console/constructor.js` writes through
  `stream.write(string, errorHandler)` in `kWriteToConsole`.
- Direct `process.stdout.write()` works in this build, but `console.log()` and
  `console.error()` produce no output. This points at the global console binding
  path, not at raw stdio.

Relevant code:

- `lib/internal/console/global.js`: global console binding at lines 34-45.
- `lib/internal/console/constructor.js`: stream binding at lines 196-235 and
  write path at lines 282-323.
- `test/fixtures/fetch-and-log.mjs`: proxy fetch fixture logs body with
  `console.log()` at lines 1-3.
- `test/fixtures/request-and-log.js`: status/header lines use `console.log()`,
  while response body uses `res.pipe(process.stdout)` at lines 35-44.

This explains why proxy tests can show outputs such as `Hello World\n` but miss
`Status Code: 200`: the body pipe works, the console status lines do not.

## Root Cause 3: `-p` / `--print` Does Not Print

Representative failures:

- `test-http-max-header-size`
- `test-tls-cipher-list`
- other child-process tests that use `-p`, `-pe`, or `--print`

Observed signatures:

- `build-edge/edge -p "1+1"` exits successfully but prints nothing.
- `test-http-max-header-size` gets empty stdout, coerces it with unary `+`, and
  observes `0 !== 10`.
- `test-tls-cipher-list` receives empty stdout where it expected
  `crypto.constants.defaultCipherList` or `tls.DEFAULT_CIPHERS`.

Why it happens:

- The CLI recognizes `-p` / `--print` and routes to
  `internal/main/eval_string`.
- `lib/internal/main/eval_string.js` passes the `print` option into
  `evalScript()`.
- The current end-to-end behavior evaluates without printing the completion
  value.

Relevant code:

- `src/edge_cli.cc`: eval/print route to `internal/main/eval_string` at lines
  1536-1545.
- `lib/internal/main/eval_string.js`: reads `--print` and passes it through at
  lines 30-77.
- `test/parallel/test-http-max-header-size.js`: expects printed
  `http.maxHeaderSize` at lines 8-11.
- `test/parallel/test-tls-cipher-list.js`: uses `-pe` for cipher checks at lines
  11-32.

The HTTP max-header and TLS cipher tests are probably not proving those option
values are wrong; they are mostly proving `--print` output is missing.

## Root Cause 4: `File` Structured Clone Loses Prototype

Representative failure:

- `test-file`

Observed signature:

```text
TypeError: clonedFile.text is not a function
```

Why it happens:

- `File` extends `Blob` and defines JS-transferable clone hooks
  `[kClone]()` / `[kDeserialize]()`.
- The structured clone implementation should detect cloneable JS-transferable
  values, create a marker, native-clone the marker payload, and deserialize back
  through `internal/file:TransferableFile`.
- In this build, `structuredClone(new File(...))` returns a plain object, so it
  has no `Blob`/`File` methods such as `.text()`.

Relevant code:

- `lib/internal/file.js`: `File` clone/deserialize hooks at lines 117-128 and
  `TransferableFile` at lines 131-137.
- `lib/internal/worker/js_transferable.js`: deserializer factory setup at lines
  33-49 and structuredClone wrapper at lines 112-127.
- `src/internal_binding/binding_messaging.cc`: cloneable-transferable detection
  at lines 669-689, marker preparation at lines 1059-1092, and restoration at
  lines 1212-1281.
- `test/parallel/test-file.js`: expected cloned `File` behavior at lines
  162-181.

## Root Cause 5: Deprecation and Node-Modules Warning Classification

Representative failures:

- `test-buffer-constructor-node-modules`
- `test-buffer-constructor-node-modules-paths`

Observed signatures:

- `--pending-deprecation` child stderr does not match `/DEP0005/`.
- Synthetic call-site tests do not see the expected
  `[DEP0005] DeprecationWarning`.

Why it happens:

- `Buffer()` warning emission depends on
  `getOptionValue('--pending-deprecation')` and
  `internalBinding('util').isInsideNodeModules()`.
- The warning path is present in JS, but the build does not emit the expected
  warning for the tested child process.
- The node-modules detection is native and stack-based; if call-site names or
  CLI option propagation differ from Node, this test flips from warning to no
  warning.

Relevant code:

- `lib/buffer.js`: warning gate and `process.emitWarning(..., 'DEP0005')` at
  lines 187-209.
- `src/edge_util.cc`: `isInsideNodeModules()` stack inspection at lines 521-579.
- `test/parallel/test-buffer-constructor-node-modules.js`: expected pending
  warning at lines 28-36.
- `test/parallel/test-buffer-constructor-node-modules-paths.js`: synthetic
  path expectations at lines 10-37.

## Root Cause 6: Inspector/Profile Flags Are Exposed Without Backing Support

Representative failures:

- `test-domain-dep0097`
- `test-diagnostic-dir-cpu-prof`
- `test-diagnostic-dir-heap-prof`
- `test-crypto-secure-heap` and heapdump/profiling-adjacent failures that load
  `node:v8`

Observed signatures:

- `inspector.open()` throws `ERR_INSPECTOR_NOT_AVAILABLE`.
- CPU/heap profiler diagnostic-dir tests expect child status 0 and profile
  files, but the child exits 1.

Why it happens:

- `internalBinding('config').hasInspector` is true, but
  `internalBinding('inspector')` is a stub whose `open()` method throws
  `ERR_INSPECTOR_NOT_AVAILABLE`.
- CLI option tables include CPU/heap profiler flags, but the implementation only
  forwards a narrow set of V8 `--prof` flags to V8 and does not implement Node's
  `--cpu-prof` / `--heap-prof` profile file lifecycle.

Relevant code:

- `src/internal_binding/binding_config.cc`: `hasInspector = true` at lines
  87-113.
- `src/internal_binding/dispatch.cc`: inspector stub methods at lines 278-335.
- `src/edge_cli.cc`: only `--prof`, `--logfile=`, and
  `--prof-sampling-interval=` are applied as supported V8 profiler flags at
  lines 253-285.
- `test/sequential/test-diagnostic-dir-cpu-prof.js`: expects a CPU profile file
  at lines 27-45.
- `test/sequential/test-diagnostic-dir-heap-prof.js`: expects a heap profile
  file at lines 70-88.

## Root Cause 7: Debug Output Does Not Match Node

Representative failures:

- `test-http-debug`
- `test-http2-debug`

Observed signatures:

- HTTP debug output includes Edge proxy-specific lines such as
  `http createConnection should use proxy ...`, changing the expected stderr.
- HTTP2 debug output contains native `HTTP2 ...` lines, but misses Node's
  sensitive-data warning:
  `Setting the NODE_DEBUG environment variable to 'http2' can expose sensitive data`.

Why it happens:

- Edge's HTTP agent has extra proxy debug logging.
- Edge's native HTTP2 binding writes directly to stderr when
  `NODE_DEBUG_NATIVE` contains `http2`.
- The test expects Node's paired JS/native debug behavior, including the warning
  emitted through Node's debuglog path.

Relevant code:

- `lib/_http_agent.js`: proxy debug line at lines 236-240.
- `src/internal_binding/binding_http2.cc`: native HTTP2 debug writes at lines
  321-323 and 402-415.
- `test/parallel/test-http2-debug.js`: expected sensitive-data warning and
  native lines at lines 10-30.

## Root Cause 8: StringDecoder Does Not Surface `ERR_STRING_TOO_LONG`

Representative failure:

- `test-string-decoder`

Observed signature:

- Large `StringDecoder().write(Buffer.alloc(...))` does not throw
  `ERR_STRING_TOO_LONG`.

Why it happens:

- `lib/string_decoder.js` delegates decoding directly to
  `internalBinding('string_decoder').decode`.
- The native string decoder builds strings from byte ranges but does not mirror
  Node/V8's maximum string length error behavior for this path.

Relevant code:

- `lib/string_decoder.js`: `write()` delegates to native `decode()` at lines
  76-87.
- `src/edge_string_decoder.cc`: `DecodeBinding()` creates the output string at
  lines 493-598.
- `test/parallel/test-string-decoder.js`: expected `ERR_STRING_TOO_LONG` at
  lines 204-210.

## Suggested Fix Order

1. Fix `internalBinding('config').hasInspector` / `profiler` consistency.
   Either expose `hasInspector = false` for this build or provide a stable
   `profiler` binding with no-op `takeCoverage` / `stopCoverage` semantics where
   appropriate. This should collapse the largest failure cluster.
2. Fix global `console.*` output before investigating proxy tests. The proxy
   tests depend heavily on console output from child fixtures.
3. Fix `-p` / `--print` result printing. Recheck `test-http-max-header-size` and
   `test-tls-cipher-list` after that before touching HTTP parser or TLS cipher
   logic.
4. Fix `File` structured clone by ensuring the JS-transferable marker path is
   used and restored for `File`.
5. Revisit deprecation warning stack classification after the console and print
   fixes, because warning tests are sensitive to both stderr and call-site
   formatting.
6. Decide policy for unsupported inspector/profile features: hide/skip them via
   config and feature flags, or implement enough stubs to satisfy Node's
   user-visible contracts.
7. Normalize HTTP/HTTP2 debug output to Node's expected warning and line shape,
   or mark Edge-specific extra diagnostics outside `NODE_DEBUG`/test-visible
   stderr.
8. Add a native string length guard in the string decoder path.

## Verification Targets

After each fix, use targeted tests before the full category run:

```sh
build-edge/edge -e "console.log('hello')"
build-edge/edge -p "1 + 1"
build-edge/edge test/parallel/test-console-count.js
build-edge/edge test/parallel/test-file.js
build-edge/edge test/parallel/test-buffer-constructor-node-modules.js
build-edge/edge test/parallel/test-http-max-header-size.js
build-edge/edge test/parallel/test-tls-cipher-list.js
build-edge/edge --test-reporter=test/common/test-error-reporter.js --test-reporter-destination=stdout test/parallel/test-events-add-abort-listener.mjs
```

Then rerun the original `make test-only TEST_JOBS=4` command.

## May 7, 2026 QuickJS Native CI Follow-Up

A later `test-and-build-quickjs / build-macos` run showed a newer failure shape
against `build-edge-quickjs-cli/edge`. The largest new clusters had two shared
causes:

1. HTTP/2 sessions failed during native handle setup with:

   ```text
   TypeError: no setter for property
       at Http2Session (native)
       at setupHandle (...)
   ```

   `src/internal_binding/binding_http2.cc` defined `fields` as a getter-only
   class accessor. The native constructor also attempted to assign
   `self.fields = fields_ta`. V8 tolerates the surrounding Node shape, but
   QuickJS reports an inherited accessor-without-setter assignment as a pending
   exception. Removing the constructor-side assignment keeps the getter-backed
   `fields` property and avoids poisoning all HTTP/2 session construction.

2. Several byte-oriented paths produced comma-separated decimal bytes instead
   of Buffer strings. Examples included `test-dgram-pingpong` observing
   `"80,73,78,71"` instead of `"PING"` and TLS off-thread certificate-loading
   tests matching stderr against byte lists instead of text.

   QuickJS `napi_create_buffer*()` returned a marked `Uint8Array`, but it did
   not adopt the runtime `Buffer.prototype`. Once Node bootstrap has called
   `internalBinding('buffer').setBufferPrototype(Buffer.prototype)`, native
   N-API buffers should use that prototype so JS-visible methods such as
   `toString()` behave like Node Buffers. The QuickJS backend now installs
   `globalThis.Buffer.prototype` on native-created buffers when available.

   This prototype install is intentionally best-effort. Native buffer creation
   has already succeeded by the time this compatibility step runs. During early
   bootstrap, or if user code has replaced or damaged `globalThis.Buffer`, the
   N-API call should still return a usable Uint8Array-backed buffer. QuickJS
   keeps exceptions pending, so failures in this optional lookup/prototype path
   are cleared instead of being returned and causing an otherwise successful
   `napi_create_buffer*()` call to fail.

Focused verification after the fixes:

```sh
cmake --build build-edge-quickjs-cli --target edge -j4
build-edge-quickjs-cli/edge -e "console.log(Buffer.from('PING').toString()); console.log(Buffer.isBuffer(Buffer.alloc(1)));"
build-edge-quickjs-cli/edge test/sequential/test-dgram-pingpong.js
build-edge-quickjs-cli/edge test/parallel/test-http2-too-many-settings.js
build-edge-quickjs-cli/edge test/parallel/test-tls-off-thread-cert-loading.js
build-edge-quickjs-cli/edge test/parallel/test-tls-off-thread-cert-loading-system.js
```

The local sandbox blocks socket binds with `EPERM`, so the dgram and HTTP/2
focused tests were rerun outside the sandbox. They passed after the native
QuickJS rebuild.

Residual failures seen during the same triage:

- `test-buffer-isascii` and `test-buffer-isutf8` still fail because
  `structuredClone(arrayBuffer, { transfer: [arrayBuffer] })` does not detach
  the original ArrayBuffer under the current QuickJS path. A direct probe showed
  `ab.byteLength` remains nonzero and `new Uint8Array(ab)` still succeeds after
  transfer.
- `test-buffer-creation-regression`, `test-buffer-alloc`, and
  `test-buffer-constants` still expose QuickJS built-in allocation limit and
  error-message differences rather than the native Buffer prototype issue.

## May 15, 2026 QuickJS-Only Failure Triage

Command:

```sh
make test-quickjs-only
```

The first interactive run exited early at about 59% because the Python harness
hit a `UnicodeDecodeError` while reading invalid UTF-8 from a failing test's
stderr. A redirected rerun completed and failed with:

```text
[01:47|%  99|+ 1632|- 124]: Done
make: *** [test-quickjs-only] Error 1
```

Failure count by broad path/topic:

```text
40 http2
20 tls
15 stream/string-decoder
13 fetch/proxy
10 WHATWG URL
7 crypto/webcrypto
5 buffer
4 dns
3 console
3 domain
4 other
```

Largest numerical clusters are HTTP/2 and TLS, but they are probably not the
smallest fixes. The best small-change wins from this run are:

1. Normalize QuickJS symbol-to-string TypeError text. Seven
   `URLSearchParams` tests fail only because QuickJS says
   `cannot convert symbol to string` while Node expects
   `Cannot convert a Symbol value to a string`. This is likely a tiny vendored
   QuickJS message patch near `JS_ToStringFree`.
2. Add a minimal `globalThis.WebAssembly` compatibility surface or route
   Undici away from its wasm `llhttp` loader. Five fetch/proxy tests fail with
   `ReferenceError: WebAssembly is not defined`, and that blocks basic
   `fetch()`/proxy coverage before response behavior can be assessed.
3. Fix URL/USVString lone-surrogate replacement. This directly affects
   `test-whatwg-url-custom-parsing` and is probably shared with the
   Buffer/StringDecoder lone-surrogate failures.
4. Fix proxy tunnel error string ordering and matchability. Several HTTPS proxy
   failure tests already produce `ERR_PROXY_TUNNEL`, but the captured stderr
   has the code after the message/body, so regexes such as
   `/ERR_PROXY_TUNNEL.*Connection .../` miss and some tests then dereference a
   null match.
5. Add Buffer-facing 32-bit allocation/string length guards. The remaining
   large-buffer failures (`test-buffer-tostring-4gb`,
   `test-buffer-creation-regression`, `test-buffer-constants`) still reach
   QuickJS's raw `invalid array buffer length` or miss Node's
   `Invalid string length` surface.

The HTTP/2 and TLS groups should be sampled after the above wins. Many entries
may share one or two native binding or socket lifecycle causes, but the log
mostly shows command-level failures without enough repeated text to rank them
above the smaller message/compatibility fixes.

## 2026-05-15 QuickJS Node-Compat Fix Pass

Implemented three of the small-change wins in vendored QuickJS/native binding
code without touching `lib/`:

- Symbol-to-string coercion now uses Node's
  `Cannot convert a Symbol value to a string` text.
- Normal QuickJS UTF-8 export replaces unmatched UTF-16 surrogates with U+FFFD,
  fixing both URL percent-encoding and Buffer UTF-8 bytes.
- QuickJS string/ArrayBuffer limit behavior now reports Node-compatible
  `Invalid string length` and accepted allocation-failure text, with
  `buffer.kStringMaxLength` set to QuickJS's actual `0x3fffffff` ceiling.

Targeted URL/SearchParams and Buffer limit tests passed. A full redirected run
improved the suite from `1632` passing / `124` failing to:

```text
[01:45|% 100|+ 1644|- 113]: Done
make: *** [test-quickjs-only] Error 1
```

The remaining failures are outside this fix pass, mostly the known HTTP/2, TLS,
proxy/fetch, stream/StringDecoder, diagnostics, crypto, domain, and URL inspect
groups.

## 2026-05-15 Post HTTP/2 and TLS Reassessment

After the QuickJS N-API object/external classification fix, the HTTP/2 crash
cluster and much of the old TLS-looking surface changed significantly. A fresh
TLS-only category run now passes completely:

```sh
NODE_TEST_RUNNER=build-edge-quickjs-cli/edge ./test/nodejs_test_harness --category=node:tls
```

Result:

```text
195/195 passed
```

The earlier `20 tls` count was a raw path/category count from the `124`-failure
triage. It was not a de-duplicated TLS root-cause count. Several entries in that
old category were fan-out from non-TLS issues, especially:

- QuickJS N-API wrapped class instances being reported as `napi_external`.
- Missing `-p` / `--print` output affecting child-process assertions such as
  `test-tls-cipher-list`.
- Missing profiler/worker/context behavior reached through test harness helpers.
- General callback/lifetime ordering differences that were later reduced by the
  HTTP/2 external-classification fix.

By the time the TLS SecureContext/SNI fix started, the live TLS category had
already been reduced to two real failures:

```text
test/parallel/test-tls-external-accessor.js
test/parallel/test-tls-server-setkeycert.js
```

The TLS fix addressed those two roots:

- inherited N-API wrap metadata incorrectly unwrapped through prototypes;
- `setKeyCert()` / SNI-selected `SecureContext.context` was not retained while
  OpenSSL still used its `SSL_CTX`.

A full `make test-quickjs-only` run after the HTTP/2 and TLS fixes completed
with:

```text
[01:50|% 100|+ 1720|-  37]: Done
```

The remaining failed-test list has no TLS entries. Current remaining buckets:

```text
8 stream/FastUTF8/async-iterator/destroy behavior
7 crypto/webcrypto/worker-thread or cross-realm behavior
6 fetch/proxy/Undici/WebAssembly or URL validation behavior
3 dns
3 domain/promise
3 http2
2 console/inspect/TTY color stack formatting
2 diagnostics-channel/promise or worker-thread behavior
1 buffer deprecation node_modules path filtering
1 os userinfo getter error handling
1 http keep-alive timeout race
```

The HTTP/2 category is no longer the old crash bucket either. A fresh
HTTP/2-only category run now has two failures:

```text
test/parallel/test-http2-response-splitting.js
test/parallel/test-http2-reset-flood.js
```

`test-http2-origin.js` appears in the full-suite failed list but not in the
HTTP/2-only category run, so treat it as order/timing-sensitive until reproduced
standalone with logs.

Revised assessment:

1. Do not expect another TLS-native pass to yield 20 wins; TLS is currently
   closed under the category runner.
2. The remaining highest-return native work is no longer TLS/SNI. It is likely
   one of:
   - WebAssembly/Undici fetch compatibility, if the decision changes to stop
     skipping Wasm-backed paths;
   - stream destroy/async iterator/FastUTF8 behavior;
   - promise hooks/context propagation for diagnostics/domain, if no longer
     deferred;
   - HTTP/2 response validation/reset-flood semantics, now only a small
     non-crash bucket.
3. For strict "no Wasm, no promise hooks/contextify" prioritization, the best
   next target is the stream/FastUTF8 bucket, followed by the small HTTP/2
   semantic bucket.

## 2026-05-15 Edge Build And CI Scope Update

The root build targets still compile the selected N-API provider. They no longer
build or run duplicate N-API test suites when exercising normal Edge runtime
CI paths.

Clean-directory build verification:

```sh
rm -rf /tmp/edgejs-ci-build-v8 /tmp/edgejs-ci-build-quickjs
make build BUILD_DIR=/tmp/edgejs-ci-build-v8 CMAKE_BUILD_TYPE=Release JOBS=4
make build-edge-quickjs-cli BUILD_EDGE_QUICKJS_CLI_DIR=/tmp/edgejs-ci-build-quickjs CMAKE_BUILD_TYPE=Release JOBS=4
```

Both builds passed. The V8 build produced:

```text
/tmp/edgejs-ci-build-v8/edge
/tmp/edgejs-ci-build-v8/edgeenv
/tmp/edgejs-ci-build-v8/napi-v8/libnapi_v8.a
```

The QuickJS build produced:

```text
/tmp/edgejs-ci-build-quickjs/edge
/tmp/edgejs-ci-build-quickjs/edgeenv
/tmp/edgejs-ci-build-quickjs/quickjs/libqjs.a
/tmp/edgejs-ci-build-quickjs/napi-quickjs/libnapi_quickjs.a
```

The relevant CMake cache entries show provider compilation with N-API tests
disabled:

```text
EDGE_BUILD_NAPI_TESTS=OFF
NAPI_V8_BUILD_TESTS=OFF
NAPI_QUICKJS_BUILD_TESTS=OFF
```

Smoke checks:

```sh
/tmp/edgejs-ci-build-v8/edge --version
/tmp/edgejs-ci-build-quickjs/edge --version
/tmp/edgejs-ci-build-quickjs/edge -e "console.log('quickjs smoke ok')"
```

Both version commands reported `v24.13.2-pre`, and the QuickJS `-e` smoke
printed `quickjs smoke ok`.

QuickJS CI/Makefile interpretation:

- `make build-edge-quickjs-cli` builds the QuickJS-backed Edge runtime,
  `libqjs.a`, and `libnapi_quickjs.a` with `EDGE_BUILD_NAPI_TESTS=OFF`.
- EdgeJS workflows should not run `test-napi*`, `test-native-*`, standalone
  N-API Cargo test jobs, or the old `napi_wasmer` host-import smoke path. Those
  suites belong in the N-API repository.

Current QuickJS runtime-test baseline remains:

```text
QuickJS: make test-quickjs-only -> 1720 passed / 37 failed
```

For the next QuickJS compatibility pass, keep skipping Wasm and
promise-hooks/contextify buckets unless that priority changes. Under that
constraint, the highest-return remaining targets are:

1. stream/FastUTF8/async-iterator/destroy behavior;
2. the now-small HTTP/2 semantic bucket;
3. console/inspect/TTY color stack formatting;
4. Buffer deprecation node-modules path filtering and other one-off native
   parity fixes.

## 2026-05-15 QuickJS CI Matrix Simplification

The active QuickJS CI workflow shape now matches the intended build/test matrix
without duplicate commented jobs or duplicate QuickJS WASIX workflows.

QuickJS build lanes that generate binaries/packages:

| Engine | Target OS | Workflow job | Command |
| --- | --- | --- | --- |
| QuickJS | Linux | `quickjs-linux` | `make build-edge-quickjs-cli` |
| QuickJS | macOS | `quickjs-macos` | `make build-edge-quickjs-cli` |
| QuickJS | WASIX | `quickjs-wasix` | `make build-quickjs-wasix` |

QuickJS runtime test lanes:

| Engine | Host OS | Workflow job | Command |
| --- | --- | --- | --- |
| QuickJS | Linux | `quickjs-linux` | `make test-quickjs-only` |
| QuickJS | macOS | `quickjs-macos` | `make test-quickjs-only` |

QuickJS publish lane:

| Engine | Workflow job | Gate | Publishes |
| --- | --- | --- | --- |
| QuickJS | `publish-nightly` | `push` to `main` after `metadata`, `quickjs-linux`, `quickjs-macos`, and `quickjs-wasix` pass | `edge-quickjs-linux-amd64`, `edge-quickjs-darwin-arm64`, `edge-quickjs-wasix`, and the QuickJS WASIX package |

The standalone `.github/workflows/napi-wasmer-quickjs.yml` workflow was removed
because QuickJS WASIX is already covered by
`.github/workflows/test-and-build-quickjs.yml`.

## 2026-05-16 QuickJS macOS CI Failure Log

The `test and build / quickjs-macos` log captured in
`test-failures-qjs-2.log` still matches the expected QuickJS compatibility
baseline shape: the CI job built and packaged the QuickJS macOS CLI, then
`make test-quickjs-only TEST_JOBS=4` finished with 1742 passing tests and 37
failures.

Grouped by likely source-level cause:

- Buffer deprecation filtering: `test-buffer-constructor-node-modules-paths.js`
  emitted the `Buffer()` deprecation warning from a node_modules fixture path.
- Console/inspect formatting: `test-console-issue-43095.js` still throws on a
  revoked proxy during inspection; `pseudo-tty/console_colors.js` still differs
  in stack/color formatting.
- DNS/c-ares lifecycle: `test-dns-multi-channel.js` produced `EDESTRUCTION`;
  `test-dns-channel-timeout.js`, `test-dns-setserver-when-querying.js`, and
  `test-dns-resolver-max-timeout.js` crashed with signal 6.
- Fetch/proxy/Undici parser bootstrap:
  `test-http-proxy-fetch.mjs`, `test-use-env-proxy-cli-http.mjs`,
  `test-fetch.mjs`, `test-https-proxy-fetch.mjs`, and
  `test-use-env-proxy-cli-https.mjs` failed because Undici tried to initialize
  its WebAssembly llhttp path while QuickJS has no global `WebAssembly`.
- HTTP proxy URL validation/timer: `test-http-proxy-request-invalid-char-in-url.mjs`
  timed out.
- Diagnostics/workers: `test-diagnostics-channel-worker-threads.js` timed out.
- Sync waiting on the main thread: `test-http-keep-alive-timeout-race-condition.js`,
  `test-fastutf8stream-flush-sync.js`, and `test-fastutf8stream-retry.js` failed
  with `TypeError: cannot block in this thread`.
- Explicit resource management syntax: `test-stream-duplex-destroy.js`,
  `test-stream-readable-dispose.js`, `test-stream-transform-destroy.js`, and
  `test-stream-writable-destroy.js` failed while parsing `using`.
- Stream semantics: `test-stream-readable-async-iterators.js` hit a destroy
  assertion and `test-stream-pipeline.js` called a non-function iterator helper.
- Crypto/WebCrypto/worker transfer:
  `test-crypto-dh-modp2-views.js`, `test-crypto-key-objects-messageport.js`,
  `test-crypto-subtle-cross-realm.js`, `test-webcrypto-digest.js`,
  `test-crypto-prime.js`, `test-crypto-worker-thread.js`, and
  `test-webcrypto-cryptokey-workers.js` still fail by a mix of bad DH view
  handling, MessagePort/key clone semantics, crashes, and worker timeouts.
- Domain/promise context: `test-domain-vm-promise-isolation.js` still observes
  the active domain where Node expects `undefined`; `test-domain-multiple-errors.js`
  timed out.
- HTTP/2 follow-up:
  `test-http2-client-set-priority.js` and
  `test-http2-compat-serverresponse-writehead.js` crashed with signal 11,
  `test-http2-response-splitting.js` failed a response-splitting assertion, and
  `test-http2-reset-flood.js` timed out. This reopens
  `troubleshooting/node-test/017_http2_native_lifecycle_crashes.md` for the new
  QuickJS macOS crash cluster.

Local reproduction was blocked in this checkout because
`napi/quickjs/deps/quickjs` is empty, so `make build-edge-quickjs-cli JOBS=4`
failed during CMake configure before producing `build-edge-quickjs-cli/edge`.
Repopulate the QuickJS dependency/submodule before rerunning the targeted tests
and the crash cases under LLDB.

## 2026-05-16 Linux QuickJS Double-Free Reproduction

The downloaded CI logs in `/Users/syrusakbary/Downloads/logs_69318706385`
show that the Linux QuickJS failure spike is dominated by native allocator
aborts, not by hundreds of unrelated JavaScript assertions. The Linux log
finished around 898 passing / 881 failing tests, while the macOS log finished
around 1739 passing / 40 failing tests. The Linux log contains hundreds of:

```text
free(): double free detected in tcache 2
double free or corruption (!prev)
--- CRASHED (Signal: 6) ---
```

Native Linux arm64 Docker reproduced the CI crash shape with one HTTP test:

```sh
docker run --rm --platform linux/arm64 \
  -v "$PWD":/work -w /work ubuntu:latest \
  bash -lc './build-edge-quickjs-cli-linux-arm64/edge \
    ./test/parallel/test-http-agent-no-protocol.js'
```

The test printed the same glibc failure:

```text
free(): double free detected in tcache 2
```

The initial suspicion was that network read buffers handed to QuickJS as
external ArrayBuffers were being finalized twice. GDB disproved that as the
direct crashing path for this test. The second free happens while destroying
the native `TcpWrap`, not while finalizing an external ArrayBuffer backing
store.

Observed sequence under GDB:

1. `EdgeStreamBaseOnClosed()` starts for the TCP stream with
   `closed = false`, `closing = true`, `delete_on_close = false`, and
   `finalized = false`.
2. While the close callback is still active, QuickJS frees the JS wrapper via
   `napi_external__::finalizer()` / `free_zero_refcount()`.
3. That calls `EdgeStreamBaseFinalize()`, which sees `closed = true`, clears the
   wrapper reference, and calls `DestroyBase()`.
4. `DestroyBase()` calls `TcpDestroy()`, which deletes the native `TcpWrap`.
5. Control returns to the still-running `EdgeStreamBaseOnClosed()` call. Because
   `base->finalized` is now true, line 1000 calls `DestroyBase()` again on the
   same `TcpWrap`, and glibc aborts with a double free.

The key backtrace for the first destroy:

```text
TcpDestroy
napi_external__::finalizer
free_object
free_gc_object
free_zero_refcount
js_free_value_rt
napi_ref__::~napi_ref__
```

The second destroy then comes from:

```text
DestroyBase
EdgeStreamBaseOnClosed
uv__finish_close
uv__run_closing_handles
uv_run
```

This explains why Linux CI reports hundreds of failures: any HTTP/HTTPS/TLS or
HTTP/2 test that creates TCP stream wrappers can hit the same native lifecycle
bug. macOS does not report the same broad failure spike because its allocator
does not abort on this exact double-delete pattern in the same way, and V8 does
not expose the same reentrant wrapper-finalizer timing as the QuickJS N-API
backend.

The implemented stream fix makes `EdgeStreamBase` destruction safe across the
close callback and wrapper finalizer paths:

- `EdgeStreamBaseOnClosed()` marks the close callback as active while it is
  still using the native stream object.
- `EdgeStreamBaseFinalize()` now defers native deletion to the close callback
  when finalization happens reentrantly during that callback.
- `DestroyBase()` has a one-way `destroyed` guard so multiple close/finalizer
  paths cannot call the native `destroy_self` hook twice.

After this change, the direct Linux Docker repros
`test/parallel/test-http-agent-no-protocol.js` and
`test/parallel/test-http-agent-null.js` exit successfully. A focused HTTP
sample covering agent close/keepalive/maxsockets/client abort cases also exits
successfully. The aggregate Linux arm64 Docker buckets moved to:

```text
node:http       total=411 passed=406 failed=5
node:https,tls  total=284 passed=280 failed=4
```

The first full post-fix run still reported 206 failures because HTTP/2 had a
second lifetime issue. A child-process GDB backtrace for
`test/parallel/test-http2-client-destroy.js` showed the child crashing in
`RemoveStreamFromSession()` while erasing from a freed
`Http2SessionWrap::streams` map. That was caused by QuickJS finalizing the JS
session wrapper while native streams still had deferred destroy callbacks
pending.

The implemented HTTP/2 fix holds the session wrapper reference while each
native `Http2StreamWrap` is registered in the session map, releases that
reference when the stream is removed, and defensively detaches any remaining
stream back-pointers during session finalization. After that fix, the HTTP/2
crash repro no longer segfaults; it fails only as a normal assertion mismatch
that belongs to the remaining compatibility baseline.

Final Linux arm64 Docker verification:

```text
node:http2 total=248 passed=244 failed=4
full quickjs category set total=1779 passed=1740 failed=39
```

That matches the macOS-sized residual failure set and removes the Linux-only
allocator-abort wave from the downloaded CI logs.

## 2026-06-05 WASIX Node Test Temp Isolation

A later full local `make test-wasix-quickjs-only` run showed a repeated block
failure:

```text
EEXIST: file already exists, mkdir '/workspace/test/.tmp.0'
```

This was a harness isolation issue, not independent HTTP, HTTP/2, TLS, zlib, or
stream subsystem breakage. `test/common/tmpdir.js` names the shared test temp
directory from `TEST_SERIAL_ID || TEST_THREAD_ID || '0'`. The Python Node test
harness sets per-test serial IDs, but the WASIX runner only forwarded `HOME`
and `NODE_TEST_DIR` through `wasmer run`, so guest tests fell back to `.tmp.0`.

`scripts/edge-wasix-node-runner.sh` now:

- forwards `TEST_SERIAL_ID` into the guest;
- synthesizes `wasix-<hash>-<pid>` for direct runner invocations without a
  harness-provided serial;
- sets `NODE_TEST_DIR=/tmp/edgejs-node-test`;
- mounts that temp root separately from the source tree;
- mounts a temporary empty root at `/workspace` and then mounts individual
  source directories under it instead of mounting the whole checkout as
  `/workspace`.

The default individual source mounts are configurable through
`WASIX_EDGEJS_WORKSPACE_DIRS` and currently default to
`test,lib,deps,node,assets`.

Focused verification:

```sh
bash -n /Users/sadhbh/src/dev/edgejs/scripts/edge-wasix-node-runner.sh
/Users/sadhbh/src/dev/edgejs/scripts/edge-wasix-node-runner.sh \
  -e "const tmpdir=require('/workspace/test/common/tmpdir.js'); tmpdir.refresh(); console.log(tmpdir.path);"
/Users/sadhbh/src/dev/edgejs/scripts/edge-wasix-node-runner.sh \
  /Users/sadhbh/src/dev/edgejs/test/parallel/test-url-format.js
```

The tmpdir probe resolved under
`/tmp/edgejs-node-test/.tmp.wasix-...`, and two concurrent direct probes used
different pid-suffixed temp directories. `parallel/test-url-format.js` passed
through the WASIX runner.
