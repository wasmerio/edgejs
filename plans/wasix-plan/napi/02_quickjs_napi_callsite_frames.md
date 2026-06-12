# QuickJS/N-API Callsite Frames

Why this is a problem:

Some Node internals inspect callsites to decide whether code is running inside
`node_modules`. If the QuickJS N-API path cannot expose enough callsite
information, behavior differs from V8.

When it occurs:

- `test-buffer-constructor-node-modules-paths`;
- stack-dependent module and error behavior.

Minimal manifestation:

```js
const err = new Error();
Error.captureStackTrace(err);
console.log(err.stack);
```

Proposed solution:

Expose QuickJS callsite data through the N-API callsite hooks in the same shape
expected by EdgeJS internal helpers.

## Proposed Solution References

Proposed solution can be found in:

**Reference commits mentioned in the recovery notes:**

```text
quickjs 41b00d4 Added callsites into QuickJS
napi 5a5c3b2 Improved quickjs callsites similar to v8
```
