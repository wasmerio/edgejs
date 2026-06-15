# QuickJS: WASI stack limit should match non-WASI QuickJS

Why this is a problem:

QuickJS should own JavaScript stack overflow behavior. If the WASI build disables
QuickJS stack limits, deep JavaScript recursion falls through to the Wasm/native
runtime stack and appears as `RuntimeError: call stack exhausted`. Native
QuickJS/EdgeJS reports the JS-level stack overflow path instead, so Node tests
that assert stack/error behavior fail only on WASIX.

This occurs in console, error, X509, and ttywrap tests that intentionally stress
stack handling or expect Node to catch/report stack overflow cleanly.

Minimal Example:

```js
function recurse() {
  return recurse();
}

try {
  recurse();
} catch (err) {
  if (!/stack|recursion|call/i.test(String(err && err.message))) {
    throw err;
  }
}
```

Representative failing tests:

```text
parallel/test-console-log-throw-primitive
parallel/test-console-no-swallow-stack-overflow
parallel/test-console-sync-write-error
parallel/test-x509-escaping
parallel/test-ttywrap-stack
```

Callgraph and boundary:

Current problematic path:

```text
JavaScript recursive/error-heavy path
  -> QuickJS function calls grow the JS stack
  -> WASI-specific QuickJS setup has stack_limit disabled
     HERE IS THE PROBLEM: QuickJS does not stop recursion at its JS stack limit,
     so execution falls through to Wasm runtime stack exhaustion.
  -> Wasmer reports RuntimeError: call stack exhausted
  -> Node test sees the wrong error surface
```

The boundary is QuickJS <-> runtime. Wasmer can report Wasm stack exhaustion, but
QuickJS should prevent normal JS recursion from reaching that boundary.

Proposed solution:

Use the same QuickJS stack-limit mechanism for WASI as for non-WASI builds. The
WASI port may need a careful stack-base setter, but it should not special-case
`rt->stack_limit = 0` for ordinary EdgeJS execution.

Relevant QuickJS code paths:

```text
~/src/edgejs/napi/quickjs/deps/quickjs/quickjs.c
~/src/edgejs/napi/quickjs/deps/quickjs/quickjs.h
~/src/edgejs/napi/quickjs/src/js_native_api_quickjs.cc
```

Proposed callgraph:

```text
JavaScript recursive/error-heavy path
  -> QuickJS function calls grow the JS stack
  -> QuickJS checks configured stack limit
  -> QuickJS raises a JS-level stack overflow/internal error
  -> EdgeJS/Node error handling observes the same class of behavior as native
```

This belongs in QuickJS because the JS engine owns stack accounting and the
shape of JavaScript stack overflow errors.

## Proposed Solution References

### Commits without PR

- Sadhbh: quickjs [9a59f17](https://github.com/wasmerio/quickjs/commit/9a59f17a026ebc3b6932afa886c21ebf02689a2e) Set WASI stack limit same as non-WASI
