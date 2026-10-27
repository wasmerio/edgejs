'use strict';

// Regression test: process.exit() must not crash while workers are exiting.
//
// process.exit() stops every worker: it terminates the worker's execution
// from the main thread. Workers that are already exiting on their own release
// their env at the same time. The main thread used to read the worker's env,
// drop the worker lock, and then terminate it, so it could terminate an
// isolate the worker thread was disposing (SIGSEGV in
// v8::internal::StackGuard::RequestInterrupt).
//
// Workers start one at a time and wait; then all of them exit together while
// the main thread calls process.exit(). Starting them one at a time keeps the
// test about the exit path rather than concurrent worker startup.
//
// The race is narrow, so this runs it in several child processes and fails if
// any of them does not exit cleanly. It uses spawnSync because the async
// 'exit' event does not report a terminating signal yet. Self-contained (no
// node-test harness); a non-zero exit signals failure.

const assert = require('assert');
const { spawnSync } = require('child_process');

const WORKERS = 12;
const RUNS = 30;

if (process.argv[2] === 'child') {
  const { Worker } = require('worker_threads');
  const workers = [];
  const start = (i) => {
    const worker = new Worker(
      `const { parentPort } = require('worker_threads');
       parentPort.once('message', () => setTimeout(() => {}, ${i % 4}));
       parentPort.postMessage('ready');`,
      { eval: true });
    workers.push(worker);
    worker.once('message', () => {
      if (i + 1 < WORKERS) return start(i + 1);
      for (const w of workers) w.postMessage('exit');
      setTimeout(() => process.exit(0), 1);
    });
  };
  start(0);
  return;
}

const failed = [];
for (let i = 0; i < RUNS; i++) {
  const { status, signal, error } = spawnSync(process.execPath, [__filename, 'child'],
                                             { stdio: 'inherit' });
  if (error || signal !== null || status !== 0) {
    failed.push({ run: i, status, signal, error: error && error.message });
  }
}
assert.deepStrictEqual(failed, [],
                       `${failed.length} of ${RUNS} runs failed: ${JSON.stringify(failed)}`);
console.log(`worker-exit-stop-race: ${RUNS} runs passed`);
