# Engine integration

Owner: coordinator. Status: local integration verified; PR CI pending.
Dependencies: fork audit. Write ownership: nested QuickJS repository only.

Merged upstream `6d46d07d04041b40f4f49eaa7fdebe44c314c699` into fork
`7b8ab77b1c4444173b2a58472a4741e56b6c1e04` as `ed3bfa5`.
`git merge-base --is-ancestor upstream/master HEAD` succeeds, and
`git rev-list --count HEAD..upstream/master` returns zero.

Resolved source conflicts by restoring upstream coroutine ownership coherently,
then retaining the fork's promise identity lookup with JSAsyncFunctionData.
Kept upstream module-attribute parser/equality logic and host import phases.
Updated JS_GetRefCount for arena allocation. Removed JS_GetArrayBufferFreeInfo.
Combined Error internal storage with the fork's lazy prepareStackTrace callback.

Verification: Debug and ASAN/UBSAN 117 tests; ASAN GC stress 116 tests; public
C/C++ header compilation, regexp runner and standalone hello compilation/run.
Logs remain under `/private/tmp/quickjs-upstream-sync/quickjs-*.log`.
Upstream whitespace warnings in the imported diff were not bulk-reformatted.

The consumed N-API QuickJS pin `ff1471c` was on a separate fork branch. Merged
it as `0daca74` to retain receiver/type metadata and derived Error filtering.
Both upstream master and ff1471c are now ancestors. The five CallSite-owned
values are cleared after transfer, preserving upstream OOM safety. Final engine
Debug117, ASAN/UBSAN117, GC stress116 and N-API95/95 x2 checks passed.
