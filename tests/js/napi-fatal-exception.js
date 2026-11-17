'use strict';

// napi_fatal_exception, as in Node:
// - in a worker with no 'uncaughtException' listener, the worker reports the
//   error to its parent and ends: none of the addon's caller's JavaScript
//   after the call runs;
// - when the handler itself throws, the process exits with 7
//   (kExceptionInFatalExceptionHandler) whatever process.exitCode is.
// Usage: edge napi-fatal-exception.js <path to napi_test_addon.node>

const assert = require('assert');
const { spawnSync } = require('child_process');
const { Worker } = require('worker_threads');

const addonPath = process.argv[2];
assert.ok(addonPath, 'usage: napi-fatal-exception.js <addon>');

if (process.argv[3] === 'handler-throws') {
  const addon = require(addonPath);
  process.exitCode = 28;
  process.on('uncaughtException', () => {
    throw new Error('thrown by the handler');
  });
  addon.fatal(new Error('fatal from an addon'));
  process.stdout.write('after the call\n');
  return;
}

const worker = new Worker(`
  const { parentPort } = require('worker_threads');
  const addon = require(${JSON.stringify(addonPath)});
  parentPort.postMessage('before');
  addon.fatal(new Error('fatal in a worker'));
  parentPort.postMessage('after');
`, { eval: true });
const messages = [];
let error = null;
worker.on('message', (message) => messages.push(message));
worker.on('error', (err) => { error = err; });
worker.on('exit', (code) => {
  assert.strictEqual(code, 1);
  assert.deepStrictEqual(messages, ['before']);
  assert.ok(error, 'the worker reported no error');
  assert.match(String(error.message), /fatal in a worker/);

  const child = spawnSync(process.execPath, [__filename, addonPath, 'handler-throws'],
                          { encoding: 'utf8' });
  assert.strictEqual(child.status, 7, `stderr: ${child.stderr}`);
  assert.doesNotMatch(child.stdout, /after the call/);
  assert.match(child.stderr, /thrown by the handler/);
  console.log('ok');
});
