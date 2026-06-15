# N-API QuickJS: callsite frames

Why this is a problem:

EdgeJS uses callsite frames for Node-compatible stack formatting, caller
location, module path behavior, and diagnostics. V8 exposes structured stack
frames; the QuickJS N-API backend needs an equivalent enough representation. If
QuickJS returns only a string stack or missing frame metadata, tests that inspect
callsite behavior diverge from native EdgeJS.

This occurs in console/error stack tests, diagnostics tests, and paths that need
caller file/line information.

Minimal Example:

```js
const assert = require('node:assert');

function capture() {
  const holder = {};
  Error.captureStackTrace(holder, capture);
  assert.match(holder.stack, /capture/);
  return holder.stack;
}

const stack = capture();
assert.match(stack, /\.js/);
```

Representative failing tests:

```text
parallel/test-console-log-throw-primitive
parallel/test-console-no-swallow-stack-overflow
parallel/test-console-sync-write-error
parallel/test-diagnostics-channel-process
```

Callgraph and boundary:

Current problematic path:

```text
JavaScript Error.captureStackTrace()/diagnostics path
  -> EdgeJS src/edge_util.cc asks unofficial N-API for callsites
  -> napi/quickjs/src/unofficial_napi.cc
  -> QuickJS stack data
     HERE IS THE PROBLEM: QuickJS backend lacks V8-like callsite objects or does
     not expose enough scriptName/lineNumber/columnNumber metadata to N-API.
  -> EdgeJS receives incomplete caller/frame information
```

The boundary is EdgeJS <-> unofficial N-API <-> QuickJS. EdgeJS can consume
callsites, but QuickJS/N-API must provide them.

Proposed solution:

Add QuickJS intrinsic callsite objects and N-API helpers that return arrays of
frame objects with the fields EdgeJS expects: function name, script name/source
URL, line, column, and enough methods/properties to act like V8 callsites for
Node's internal use.

Relevant N-API / QuickJS code paths:

```text
~/src/edgejs/src/edge_util.cc
~/src/edgejs/napi/include/unofficial_napi.h
~/src/edgejs/napi/quickjs/src/unofficial_napi.cc
~/src/edgejs/napi/quickjs/src/internal/napi_callsite.cc
~/src/edgejs/napi/quickjs/deps/quickjs/quickjs.c
~/src/edgejs/napi/quickjs/deps/quickjs/quickjs.h
```

Proposed callgraph:

```text
JavaScript Error.captureStackTrace()/diagnostics path
  -> EdgeJS src/edge_util.cc requests callsites
  -> unofficial_napi_get_call_sites()
  -> QuickJS JS_GetCurrentStackTrace()
  -> QuickJS builds CallSite-like objects with file/line/column/function fields
  -> EdgeJS formats/uses caller information like the V8 backend
```

This keeps stack-frame semantics in the JS engine integration, not in each
EdgeJS feature that happens to need caller metadata.
