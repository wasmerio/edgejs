# WASIX remaining Node test failures fix plan

| | | Remarks |
| --- | --- | --- |
| **Status** | ▶️ | Action plan for the 12 in-process WASIX failures left after environment exclusions. |
| **Severity** | High | These are the only failures that represent real WASIX runtime parity work in the current QuickJS lane. |

Date: 2026-06-23

## Context

After rebuilding QuickJS WASIX and running:

```sh
make build-quickjs-wasix JOBS=4
make test-wasix-quickjs-only TEST_JOBS=4
```

the lane reported **1674 passed / 64 failed**. Most failures are expected WASIX
limitations and should not be treated as fix targets:

- Unix domain sockets / named pipes
- `cluster` / `fork` / subprocess harnesses
- external shell tools (`spawnSync /bin/sh`, `exec curl`, etc.)
- home directory / process priority
- stack-overflow console tests (fixed upstream, not yet in this workspace)
- unsupported crypto (Argon2, secure heap subprocess, PQC, abort semantics)
- TLS env/keylog tests that fork a child process

This plan covers the **12 remaining tests** that still fail for in-process TCP/TLS/HTTP/DNS/FS
reasons while native QuickJS passes them today.

Verification baseline:

```sh
make build-edge-quickjs-cli JOBS=4
make test-quickjs-only TEST_JOBS=4          # native: 1738 passed / 0 failed (2026-06-23)

make build-quickjs-wasix JOBS=4
make test-wasix-quickjs-only TEST_JOBS=4    # wasix: 1674 passed / 64 failed
```

Full WASIX log from the triage run:

```text
/tmp/edgejs-test-wasix-quickjs-only.log
```

## Remaining failures

| # | Test | Observed failure | Primary owner |
| --- | --- | --- | --- |
| 1 | `client-proxy/test-http-proxy-request-connection-refused.mjs` | proxy client sees `AggregateError [ENOTCONN]` instead of refused semantics | Wasmer socket last-error + EdgeJS/libuv errno mapping |
| 2 | `client-proxy/test-https-proxy-request-connection-refused.mjs` | same as above over TLS proxy | same |
| 3 | `sequential/test-http-econnrefused.js` | error message lacks `/ECONNREFUSED/`; logs show `AggregateError` | same |
| 4 | `sequential/test-tls-connect.js` | `err.code === 'ENOTCONN'`, expected `'ECONNREFUSED'` | same |
| 5 | `parallel/test-dns-perf_hooks.js` | harness timeout | EdgeJS c-ares binding |
| 6 | `parallel/test-dns-setserver-when-querying.js` | harness timeout | EdgeJS c-ares binding |
| 7 | `parallel/test-http-writable-true-after-close.js` | `mustCall` expected 1 callback, got 2 on proxy `ServerResponse` | EdgeJS HTTP stream lifecycle |
| 8 | `parallel/test-fastutf8stream-mode.js` | `statSync(dest).mode & 0o700` is `0`, expected `384` (`0o600`) | WASIX stat mode / `wasix-libc` |
| 9 | `parallel/test-tls-alert-handling.js` | TLS alert/error assertion mismatch | EdgeJS OpenSSL error propagation |
| 10 | `parallel/test-tls-error-stack.js` | missing `opensslErrorStack`; message is `error:13000084:...::dso not found` | EdgeJS OpenSSL error propagation |
| 11 | `parallel/test-tls-hello-parser-failure.js` | TLS parser failure assertion mismatch | EdgeJS OpenSSL error propagation |
| 12 | `parallel/test-tls-junk-server.js` | expected `/packet length too long/`, got different TLS error text | EdgeJS OpenSSL error propagation |

Note: TLS bucket is four tests; network bucket is four tests; DNS is two tests; HTTP and FS are one each (12 total).

## Fix buckets

### Bucket A — connect/refused errno mapping (4 tests)

**Symptom**

Refused or aborted TCP connects surface as `ENOTCONN` or `AggregateError` instead of
Node-visible `ECONNREFUSED`. Proxy fixtures and sequential reconnect tests depend on
stable connect-error codes and message text.

**Representative signatures**

```text
AggregateError [ENOTCONN]
actual: 'ENOTCONN'
expected: 'ECONNREFUSED'
The input did not match the regular expression /ECONNREFUSED/
```

**Root cause hypothesis**

Wasmer/WASIX does not reliably preserve the original connect failure on the socket
for later `getsockopt(SO_ERROR)` / libuv read paths. EdgeJS then maps the wrong errno
into JavaScript.

**Existing upstream plan**

- [`plans/wasix-plan/wasmer/03_last_socket_error_so_error.md`](../../wasix-plan/wasmer/03_last_socket_error_so_error.md)

**Work plan**

1. Reproduce minimally under Wasmer with a nonblocking connect to a closed loopback port and inspect `SO_ERROR`.
2. Land Wasmer socket `last_error` retention and WASIX `Sockoption::LastError` behavior.
3. Re-run the four targeted tests under `scripts/edge-wasix-node-runner.sh`.
4. If errno is correct at the libc layer but still wrong in JS, inspect EdgeJS/libuv mapping in the TCP connect and write error paths only; avoid WASIX-only guest rewrites unless the lower layer fix is blocked.

**Targeted verification**

```sh
make build-quickjs-wasix JOBS=4
NODE_TEST_RUNNER=./scripts/edge-wasix-node-runner.sh \
  WASMER_BIN=wasmer EDGEJS_ROOT=$PWD WASIX_EDGEJS_PACKAGE_DIR=$PWD/quickjs-wasm \
  ./test/nodejs_test_harness \
  --tests=client-proxy/test-http-proxy-request-connection-refused.mjs,client-proxy/test-https-proxy-request-connection-refused.mjs,sequential/test-http-econnrefused.js,sequential/test-tls-connect.js \
  -j 1
```

**Done when**

All four tests pass on WASIX without changing test expectations.

---

### Bucket B — DNS resolver timeouts (2 tests)

**Symptom**

Both tests hang until the harness timeout. Native QuickJS completes them.

**Root cause hypothesis**

EdgeJS c-ares request lifecycle / resolver reconfiguration does not complete callbacks
reliably under WASIX scheduling. This is the same class called out for other DNS tests
that already pass natively but were historically flaky or slow on WASIX.

**Existing upstream plan**

- [`plans/wasix-plan/edgejs/01_c_ares_request_lifecycle_and_address_family_selection.md`](../../wasix-plan/edgejs/01_c_ares_request_lifecycle_and_address_family_selection.md)

**Work plan**

1. Run each test alone with `EDGE_TRACE_BOOTSTRAP=1` and DNS tracing if available; confirm whether c-ares callbacks never fire or fire after timeout.
2. Inspect `src/edge_cares_wrap.cc` (and related resolver channel code) for:
   - lost callbacks on `setServers()` while queries are active;
   - missing cancellation/completion on channel teardown;
   - perf_hooks integration waiting on DNS events that never publish.
3. Fix request bookkeeping in EdgeJS so every started c-ares request completes or errors on all backends.
4. Re-run the two tests natively first, then under WASIX.

**Targeted verification**

```sh
build-edge-quickjs-cli/edge test/parallel/test-dns-perf_hooks.js
build-edge-quickjs-cli/edge test/parallel/test-dns-setserver-when-querying.js

NODE_TEST_RUNNER=./scripts/edge-wasix-node-runner.sh \
  WASMER_BIN=wasmer EDGEJS_ROOT=$PWD WASIX_EDGEJS_PACKAGE_DIR=$PWD/quickjs-wasm \
  ./test/nodejs_test_harness \
  --tests=parallel/test-dns-perf_hooks.js,parallel/test-dns-setserver-when-querying.js \
  -j 1
```

**Done when**

Both tests finish within the harness timeout on WASIX with no native regression.

---

### Bucket C — HTTP `ServerResponse.writable` after proxy abort (1 test)

**Symptom**

`test-http-writable-true-after-close.js` registers one `mustCall` listener on both
`finish` and `close`, expecting exactly one of them to assert `res.writable === true`.
WASIX fires both paths (`Expected exactly 1, actual 2`).

**Root cause hypothesis**

The nested proxy setup aborts the outer client while piping an inner response. Under
WASIX, both `finish` and `close` are emitted on the proxy `ServerResponse`, whereas
native emits only one lifecycle event for the assertion path.

**Work plan**

1. Reproduce with `EDGE_TRACE_NET=1` under WASIX and capture event order on the proxy `ServerResponse`.
2. Compare with native QuickJS for the same test.
3. Inspect EdgeJS HTTP outgoing/message destroy and pipe abort handling; look for a duplicate emit when the upstream socket aborts during `inner.pipe(res)`.
4. Fix event gating so only the Node-compatible lifecycle event reaches the assertion path.

**Targeted verification**

```sh
build-edge-quickjs-cli/edge test/parallel/test-http-writable-true-after-close.js

NODE_TEST_RUNNER=./scripts/edge-wasix-node-runner.sh \
  WASMER_BIN=wasmer EDGEJS_ROOT=$PWD WASIX_EDGEJS_PACKAGE_DIR=$PWD/quickjs-wasm \
  ./test/nodejs_test_harness \
  --tests=parallel/test-http-writable-true-after-close.js \
  -j 1
```

**Done when**

The proxy response emits exactly one of `finish` or `close` for the assertion listener on WASIX.

---

### Bucket D — `Utf8Stream` file mode stat (1 test)

**Symptom**

`test-fastutf8stream-mode.js` expects `(statSync(dest).mode & 0o700) === 384`
(`0o600`). WASIX returns `0`.

**Root cause hypothesis**

WASIX/`wasix-libc` stat mode bits are not populated the way Node expects for newly
created files. This is separate from the existing FastUtf8Stream blocking-wait issue
tracked in [`troubleshooting/node-test/011_fastutf8stream_sync_wait.md`](troubleshooting/node-test/011_fastutf8stream_sync_wait.md).

**Existing upstream plan**

- [`plans/wasix-plan/edgejs/03_remove_wasix_only_guest_workarounds_after_lower_layer_plans_land.md`](../../wasix-plan/edgejs/03_remove_wasix_only_guest_workarounds_after_lower_layer_plans_land.md)

**Work plan**

1. Confirm whether the file contents are correct and only mode bits are wrong.
2. Trace `fs.stat` / `uv_fs_stat` mode translation for WASIX-created files.
3. Prefer fixing mode reporting in `wasix-libc`/Wasmer FS stat metadata.
4. Do not add a permanent EdgeJS-only mode rewrite unless lower-layer work is blocked; if a temporary guest workaround is needed, track it for removal in plan 03.

**Targeted verification**

```sh
build-edge-quickjs-cli/edge test/parallel/test-fastutf8stream-mode.js

NODE_TEST_RUNNER=./scripts/edge-wasix-node-runner.sh \
  WASMER_BIN=wasmer EDGEJS_ROOT=$PWD WASIX_EDGEJS_PACKAGE_DIR=$PWD/quickjs-wasm \
  ./test/nodejs_test_harness \
  --tests=parallel/test-fastutf8stream-mode.js \
  -j 1
```

**Done when**

Mode assertion passes on WASIX and native behavior is unchanged.

---

### Bucket E — TLS/OpenSSL error text and stacks (4 tests)

**Symptom**

In-process TLS tests fail on error message shape or missing `opensslErrorStack`, not on
missing subprocesses or unix sockets:

| Test | Expected shape | WASIX observation |
| --- | --- | --- |
| `test-tls-junk-server.js` | `/packet length too long/` (OpenSSL 3.2+) | different TLS error text |
| `test-tls-hello-parser-failure.js` | parser failure assertion | strictEqual mismatch |
| `test-tls-alert-handling.js` | alert handling assertion | strictEqual mismatch |
| `test-tls-error-stack.js` | `opensslErrorStack.length > 0`, `/could not load the shared library/` | `error:13000084:engine routines::dso not found`, empty stack |

**Root cause hypothesis**

EdgeJS OpenSSL error conversion under the vendored WASIX OpenSSL build does not match
native error queue extraction. Engine/client-cert paths may surface a different first
error than Node's OpenSSL build, and the stack collector may not walk the queue on WASIX.

**Work plan**

1. Reproduce each failure natively with `build-edge-quickjs-cli/edge` to confirm expected messages on Linux native QuickJS.
2. Under WASIX, inspect TLS error construction in EdgeJS crypto/TLS bindings and vendored OpenSSL linkage for missing error queue APIs.
3. Fix error propagation in shared EdgeJS code where possible:
   - preserve `opensslErrorStack` population for multi-error chains;
   - map engine-load failures to Node-compatible messages when the underlying OpenSSL reason differs but semantics match.
4. Only adjust message matching when OpenSSL 3.x reason strings genuinely differ and Node itself accepts both shapes; prefer fixing stack population first.

**Targeted verification**

```sh
build-edge-quickjs-cli/edge test/parallel/test-tls-junk-server.js
build-edge-quickjs-cli/edge test/parallel/test-tls-hello-parser-failure.js
build-edge-quickjs-cli/edge test/parallel/test-tls-alert-handling.js
build-edge-quickjs-cli/edge test/parallel/test-tls-error-stack.js

NODE_TEST_RUNNER=./scripts/edge-wasix-node-runner.sh \
  WASMER_BIN=wasmer EDGEJS_ROOT=$PWD WASIX_EDGEJS_PACKAGE_DIR=$PWD/quickjs-wasm \
  ./test/nodejs_test_harness \
  --tests=parallel/test-tls-junk-server.js,parallel/test-tls-hello-parser-failure.js,parallel/test-tls-alert-handling.js,parallel/test-tls-error-stack.js \
  -j 1
```

**Done when**

All four TLS tests pass on WASIX without weakening native assertions.

## Recommended execution order

| Phase | Bucket | Why first |
| --- | --- | --- |
| 1 | A — errno mapping | Unblocks four tests and reduces misleading errors in later network/TLS debugging |
| 2 | B — DNS timeouts | Independent of socket errno work; high signal for c-ares lifecycle |
| 3 | D — stat mode | Small, well-scoped FS metadata fix |
| 4 | C — HTTP writable lifecycle | Depends on stable abort/close semantics from Bucket A |
| 5 | E — TLS error stacks | Often easier to diagnose once connect/refused behavior is trustworthy |

## CI / skip-list follow-up

`Makefile` now defines `WASIX_SKIP_ENV_TESTS` (52 unique test files from the 2026-06-23
triage; grouped as unix sockets, cluster/fork, subprocess/shell, OS, stack overflow,
UDP, unsupported crypto, TLS env subprocess harnesses, and misc). `make test-wasix-quickjs-only`
passes `--skip-tests=$(QUICKJS_SKIP_TESTS),$(WASIX_SKIP_ENV_TESTS)` so the lane fails only
on the 12 in-process parity targets below (plus any new regressions).

Do not add those 12 tests to `WASIX_SKIP_ENV_TESTS` once their buckets are fixed.

### Bucket F — Wasmer runtime aborts under parallel load (flaky)

**Symptom**

Intermittent `RuntimeError: out of bounds memory access` from the Wasmer guest when the
full WASIX suite runs under harness load. The same test passes in isolation and passed on
prior CI runs (e.g. run 28187356790 green, 28227437681 failed once with `TEST_JOBS=1`).

**Example**

- `parallel/test-http2-misbehaving-flow-control-paused.js` — CI log shows Wasmer OOB stack,
  not `NGHTTP2_FLOW_CONTROL_ERROR` assertion failure.

**Mitigation**

Skip this test in the WASIX lane until Wasmer guest stability under HTTP/2 load is understood.
CI still runs WASIX node tests with `TEST_JOBS=1` in
`.github/workflows/test-and-build-quickjs.yml` to reduce parallel guest load elsewhere.

## Success criteria

Re-run:

```sh
make build-quickjs-wasix JOBS=4
make test-wasix-quickjs-only TEST_JOBS=4
```

Target state:

- native QuickJS lane remains green;
- WASIX lane passes all in-process tests covered here;
- remaining WASIX failures, if any, are only from documented environment exclusions or an explicit skip list.

## Cross references

- Historical clustering: [`009_node_test_failures_analysis.md`](009_node_test_failures_analysis.md)
- WASIX HTTP bring-up history: [`005_wasix_wasmer_http.md`](005_wasix_wasmer_http.md)
- Wasmer socket error plan: [`plans/wasix-plan/wasmer/03_last_socket_error_so_error.md`](../../wasix-plan/wasmer/03_last_socket_error_so_error.md)
- EdgeJS c-ares plan: [`plans/wasix-plan/edgejs/01_c_ares_request_lifecycle_and_address_family_selection.md`](../../wasix-plan/edgejs/01_c_ares_request_lifecycle_and_address_family_selection.md)
- Guest workaround removal plan: [`plans/wasix-plan/edgejs/03_remove_wasix_only_guest_workarounds_after_lower_layer_plans_land.md`](../../wasix-plan/edgejs/03_remove_wasix_only_guest_workarounds_after_lower_layer_plans_land.md)
