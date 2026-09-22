# QuickJS upstream synchronization compatibility

| | | Remarks |
| --- | --- | --- |
| **Status** | ▶️ | Integrating upstream 0.17.0 and validating retained embedding behavior. |
| **Severity** | Medium | Overlapping engine ownership and stack behavior require adaptation. |

## Action plan

1. Replace the fork coroutine ownership implementation with upstream's complete
   `cur_gc_obj` / `is_coro` implementation; do not combine GC representations.
2. Adapt ArrayBuffer callbacks and delete the superseded N-API detach workaround.
3. Preserve lazy `prepareStackTrace` with upstream Error internal stack storage.
   Audit found an inherited success-return check (`== 0`, while successful
   `JS_DefinePropertyGetSet` returns 1), causing eager + lazy double formatting.
   Reproduce, correct the success check, and add single-call/capture/setter tests.
4. Validate bundled engine tests under sanitizers and GC stress, N-API Release
   and Debug, and native/WASIX EdgeJS via linked PRs.

## Evidence before the stack correction

Debug qjs, inline evaluation:
`let c=0; Error.prepareStackTrace=()=>++c; const e=new Error(); print(c,e.stack,c)`
observes eager formatting during construction and again on stack access.

Direct qjs invocation of an import-bearing module also faults in
`js_inner_module_linking` with a NULL dependency (LLDB reproduced). The fork
intentionally defers module resolution to its host, so bundled tests use the
`run-test262` driver, which resolves modules explicitly. Determine baseline and
scope before changing the standalone loader; this is not evidence of an EdgeJS
loader regression.

## Integration record

See [task analysis](../../../dev_010_quickjs_upstream_sync/README.md) and its
worker notes for the complete retained/deleted inventory and validation results.

## Local verification

The final merge retains consumed `ff1471c`'s `JS_DefineProperty` / `ret == 1`
non-exotic installation and engine regression tests pass. The incorrect return
check was present on fork master but already fixed on the consumer branch.
N-API Release and Debug pass 95/95 each after the fix. Engine ASAN/UBSAN and GC
stress pass. The standalone qjs import failure was separately reproduced on
origin/master `7b8ab77`, confirming it is pre-existing. Cross-repository CI is
pending; no default branch updates have been made.

## First cross-repository CI checkpoint

QuickJS and N-API final heads `0daca74` and `54d7989` are green across all
checks. Initial EdgeJS commit `c55d9e07` (before retaining ff1471c) passed both
native QuickJS jobs but failed WASIX `test-x509-escaping` with call-stack
exhaustion (1,674 passed / 1 failed). The final local WASIX binary passes that
exact test; rerun the final EdgeJS pin before diagnosing a Linux-only failure.
V8 macOS had an HTTP2 test failure and Linux had an npm registry metadata fetch
failure. No tests were skipped or softened to bypass these gates.
