'use strict';

// Regression test: a child process killed by a signal reports that signal.
//
// The async 'exit' event used to report a child killed by SIGSEGV, SIGBUS,
// SIGFPE, SIGTRAP or SIGILL as exit(0, null): the process wrap only knew the
// names of a few signals and reported an empty name for the rest, which reads
// as "no signal". Self-contained (no node-test harness); a non-zero exit
// signals failure.

const assert = require('assert');
const { spawn, spawnSync } = require('child_process');

// Signals whose default action terminates the process and that the runtime
// does not handle itself (SIGPIPE is ignored, like in Node).
const SIGNALS = ['SIGSEGV', 'SIGBUS', 'SIGFPE', 'SIGILL', 'SIGTRAP', 'SIGALRM', 'SIGTERM', 'SIGKILL'];

function childArgs(signal) {
  return ['-e', `process.kill(process.pid, ${JSON.stringify(signal)})`];
}

for (const signal of SIGNALS) {
  const result = spawnSync(process.execPath, childArgs(signal));
  assert.strictEqual(result.status, null, `spawnSync ${signal} status`);
  assert.strictEqual(result.signal, signal, `spawnSync ${signal} signal`);
}

let pending = SIGNALS.length;
for (const signal of SIGNALS) {
  const child = spawn(process.execPath, childArgs(signal), { stdio: 'ignore' });
  child.on('exit', (code, exitSignal) => {
    assert.strictEqual(code, null, `spawn ${signal} code`);
    assert.strictEqual(exitSignal, signal, `spawn ${signal} signal`);
    assert.strictEqual(child.exitCode, null, `spawn ${signal} exitCode`);
    assert.strictEqual(child.signalCode, signal, `spawn ${signal} signalCode`);
    if (--pending === 0) console.log(`child-exit-signal: ${SIGNALS.length} signals reported`);
  });
}

process.on('exit', () => assert.strictEqual(pending, 0, 'every child reported its exit'));
