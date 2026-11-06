# V8 raw test subprocess safety

| | | Remarks |
| --- | --- | --- |
| **Status** | 🟠 | Fresh-process isolation fixes fork crashes/deadlocks; an independent intermittent IPC-delivery failure remains. |
| **Severity** | Medium | The harness can crash or deadlock while shipping CLI compatibility tests pass. |

## Observed failure and diagnosis

During the N-API realm upgrade's full native V8 CTest run, raw buffer-read-double
sporadically exits 139, DNS emits the explicit macOS Objective-C fork-safety
abort, and unrelated raw tests deadlock until the 60-second test timeout. The
buffer fixture passes when rerun under LLDB. Shipping CLI compatibility tests
do not initialize an engine in a post-fork child and continue passing.

`RunRawNodeTestScriptInSubprocess` forks after the fixture suite configures V8
and then initializes a new engine and runs C++/JavaScript directly in the child.
V8 owns background threads; their locks and Objective-C initialization state
cannot safely be inherited this way. The historical V8 integration note also
records intermittent raw REPL child crashes on both older V8 releases.

## Action plan and ownership

Test-harness worker owns `tests/runners/test_3_node_dropin_subset_phase02.cc`
and this note only. Replace post-fork engine initialization with a fresh exec
of the same gtest case, using an explicit child-mode environment marker to
execute the raw fixture without recursive spawning. Prefer `posix_spawn` with
file actions and process-group attributes, preserving stdout/stderr capture,
TTY handling, environment setup, group cleanup and per-test diagnostics. Do
not modify runtime code or weaken assertions. Other workers are active.

Verify the exact buffer, DNS and timeout cases, then repeat a focused batch
under concurrency. The primary worker will run final relevant CTest coverage.

## Implementation

The raw subprocess runner now uses `posix_spawnp` to exec its own executable
with the current gtest suite/case filter. `EDGE_RAW_NODE_TEST_CHILD` names the
script only in that fresh process, which removes the marker and runs the
original environment attachment, event-loop execution, tracked-child cleanup
and environment teardown. Gtest sharding variables are removed for the child,
because the parent has already selected its shard; inherited output/repeat
settings are overridden to prevent duplicate reports or repeated fixtures.

File actions preserve optional stdout/stderr capture. Spawn attributes create
an independent process group, retaining the existing descendant cleanup after
`waitpid`. Pseudo-TTY cases spawn the existing Python helper directly with the
same test environment and optional `.in` stdin file. No engine initializes
between `fork` and `exec`, and production runtime sources are unchanged.

## Verification

The `edge_test_3_node_dropin_subset_phase02` target builds successfully.
All nine original failed cases pass once concurrently at `ctest -j4`:
buffer-read-double, EventEmitter listener side effects, report signal, REPL
inspect defaults, HTTP client agent end/close, HTTP client read-in-error, HTTP
DNS error, dgram send callback error, and child-process IPC next-tick.

A 12-case batch at `ctest -j6 --repeat until-fail:5` adds file-backed stdio
capture and two pseudo-TTY cases. Eleven cases pass all five repetitions;
the IPC next-tick fixture passes its first three repetitions and then exposes
the separate delivery failure described below. There are no Objective-C
fork-safety aborts, raw buffer crashes, or harness deadlocks in these runs.

A sharded gtest invocation with two selected raw cases and
`GTEST_TOTAL_SHARDS=2 GTEST_SHARD_INDEX=1` confirms the selected child still
executes exactly one matching case rather than being skipped by inherited
sharding.

Logs:

- `/private/tmp/edge-raw-spawn-build.log`
- `/private/tmp/edge-raw-spawn-targeted.log`
- `/private/tmp/edge-raw-spawn-repeated.log`
- `/private/tmp/edge-raw-spawn-sharded.log`
- `/private/tmp/edge-raw-spawn-ipc-isolated.log`

`git diff --check` passes for the harness and this note.

## Remaining IPC delivery failure

`parallel/test-child-process-ipc-next-tick.js` occasionally times out after
30 seconds with active `Socket,ChildProcess,Pipe` handles. The child receives
fewer than the expected ten `uncaughtException` callbacks (two observed after
the harness fix). The original full CTest run already had this exact failure,
with three deliveries, so fresh exec does not introduce it. The other eight
original failures remain green across five repeated concurrent runs.

This is a separate runtime IPC/exception-delivery investigation. The harness
fix preserves assertions and the existing loop timeout; no production IPC
sources were changed.

The IPC case also reproduces on the second repetition of an isolated
`ctest --repeat until-fail:5` run, again delivering only two of ten callbacks.
It does not require concurrent CTest cases.

The final full Edge-owned V8 CTest run after this change records **1,431 passed,
18 existing skips, one failed** out of 1,450. Only the pre-existing IPC test
fails; all fork aborts and raw child deadlocks are gone. Log:
`/private/tmp/edge-realms-v8-spawn-ctest.log`.
