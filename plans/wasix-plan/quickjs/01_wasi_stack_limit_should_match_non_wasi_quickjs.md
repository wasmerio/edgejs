# WASI Stack Limit Should Match Non-WASI QuickJS

Why this is a problem:

If QuickJS disables its stack limit under WASI, recursive JavaScript can run
until the Wasm stack exhausts. Then the runtime reports `RuntimeError: call
stack exhausted` instead of QuickJS producing the JS-facing overflow behavior
that Node tests expect.

When it occurs:

- console recursion tests;
- `test-x509-escaping`;
- `test-ttywrap-stack`;
- any test that intentionally exercises stack overflow handling.

Minimal manifestation:

```js
function recurse() {
  return recurse();
}

try {
  recurse();
} catch (err) {
  console.log(err.name, err.message);
}
```

Boundary:

```text
JavaScript recursion
  -> QuickJS stack check
  -> JS RangeError/InternalError path
  -> EdgeJS/Node error assertion

Bad path:
JavaScript recursion
  -> no QuickJS stack limit under WASI
  -> Wasm stack exhaustion
  -> Wasmer RuntimeError
```

Proposed solution:

Remove the WASI-only `rt->stack_limit = 0` special case. Use the same stack
limit setup for WASI that non-WASI QuickJS uses.

Sketch:

```c
/* Avoid WASI-only unlimited stack behavior. */
JS_SetMaxStackSize(rt, stack_size);
```

## Proposed Solution References

Proposed solution can be found in:

**Reference commits:**

```text
quickjs 9a59f17 Set WASI stack limit same as non-WASI
```

**Related recovery note:**

```text
quickjs cec4427 Improved stack setter
```
