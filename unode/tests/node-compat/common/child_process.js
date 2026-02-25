'use strict';

const { spawnSync } = require('child_process');

function spawnSyncAndAssert(file, args, expectations = {}) {
  const result = spawnSync(file, args);
  if (result.status !== 0 || result.signal != null) {
    throw new Error(`child process failed: status=${result.status} signal=${String(result.signal)}`);
  }

  const trim = !!expectations.trim;
  const stderr = String(result.stderr || '');
  const stdout = String(result.stdout || '');
  const stderrValue = trim ? stderr.trim() : stderr;
  const stdoutValue = trim ? stdout.trim() : stdout;

  if (expectations.stderr !== undefined) {
    const expected = expectations.stderr;
    if (expected instanceof RegExp) {
      if (!expected.test(stderrValue)) {
        throw new Error(`stderr did not match ${String(expected)}: ${stderrValue}`);
      }
    } else if (stderrValue !== expected) {
      throw new Error(`stderr mismatch: got "${stderrValue}" expected "${expected}"`);
    }
  }

  if (expectations.stdout !== undefined) {
    const expected = expectations.stdout;
    if (expected instanceof RegExp) {
      if (!expected.test(stdoutValue)) {
        throw new Error(`stdout did not match ${String(expected)}: ${stdoutValue}`);
      }
    } else if (stdoutValue !== expected) {
      throw new Error(`stdout mismatch: got "${stdoutValue}" expected "${expected}"`);
    }
  }

  return { child: result, stderr: stderrValue, stdout: stdoutValue };
}

module.exports = {
  spawnSyncAndAssert,
};
