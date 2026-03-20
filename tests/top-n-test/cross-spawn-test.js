'use strict';

const assert = require('node:assert/strict');

(async () => {
  const spawn = require('cross-spawn');

  // Sync: run node -e and capture stdout
  const syncResult = spawn.sync(process.execPath, ['-e', 'process.stdout.write("42")'], {
    encoding: 'utf8',
  });
  assert.equal(syncResult.status, 0);
  assert.equal(syncResult.stdout, '42');
  assert.equal(syncResult.error, null);

  // Sync: run node -e with JSON output
  const syncJson = spawn.sync(process.execPath, ['-e', 'console.log(JSON.stringify({a:1}))'], {
    encoding: 'utf8',
  });
  assert.equal(syncJson.status, 0);
  assert.deepEqual(JSON.parse(syncJson.stdout.trim()), { a: 1 });

  // Sync: process that exits with non-zero code
  const syncFail = spawn.sync(process.execPath, ['-e', 'process.exit(2)'], {
    encoding: 'utf8',
  });
  assert.equal(syncFail.status, 2);

  // Async: spawn node -e and read stdout via events
  const asyncOutput = await new Promise((resolve, reject) => {
    const child = spawn(process.execPath, ['-e', 'process.stdout.write("async-ok")']);
    let stdout = '';
    child.stdout.on('data', (chunk) => { stdout += chunk.toString(); });
    child.on('error', reject);
    child.on('close', (code) => {
      if (code !== 0) {
        reject(new Error(`exit code ${code}`));
        return;
      }
      resolve(stdout);
    });
  });
  assert.equal(asyncOutput, 'async-ok');

  // Async: capture stderr
  const stderrOutput = await new Promise((resolve, reject) => {
    const child = spawn(process.execPath, ['-e', 'process.stderr.write("warn!")']);
    let stderr = '';
    child.stderr.on('data', (chunk) => { stderr += chunk.toString(); });
    child.on('error', reject);
    child.on('close', () => resolve(stderr));
  });
  assert.equal(stderrOutput, 'warn!');

  // Sync: handle a command that doesn't exist
  const badCmd = spawn.sync('this-command-does-not-exist-xyz', [], { encoding: 'utf8' });
  assert.ok(badCmd.error, 'should have an error for non-existent command');
  assert.equal(badCmd.error.code, 'ENOENT');

  console.log('cross-spawn-test:ok');
})().catch((err) => {
  console.error('cross-spawn-test:fail');
  console.error(err && err.stack ? err.stack : String(err));
  process.exit(1);
});
