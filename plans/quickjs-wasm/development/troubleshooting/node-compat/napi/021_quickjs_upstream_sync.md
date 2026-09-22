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

The lazy success check is corrected (`> 0`) and engine regression tests pass.
N-API Release and Debug pass 95/95 each after the fix. Engine ASAN/UBSAN and GC
stress pass. The standalone qjs import failure was separately reproduced on
origin/master `7b8ab77`, confirming it is pre-existing. Cross-repository CI is
pending; no default branch updates have been made.
