'use strict';

const assert = require('node:assert/strict');
const { spawn } = require('node:child_process');
const { once } = require('node:events');

async function main() {
  const started = Date.now();
  const child = spawn(process.execPath, [
    '-e',
    'setTimeout(() => { console.log("CHILD_READY"); process.exit(23); }, 120)',
  ], { stdio: ['ignore', 'pipe', 'inherit'] });
  let stdout = '';
  child.stdout.on('data', (chunk) => { stdout += chunk; });

  let timer;
  try {
    const timeout = new Promise((_, reject) => {
      timer = setTimeout(() => {
        child.kill();
        reject(new Error('child exit was not observed'));
      }, 5000);
    });
    const [code, signal] = await Promise.race([once(child, 'close'), timeout]);
    assert.equal(code, 23);
    assert.equal(signal, null);
    assert.match(stdout, /CHILD_READY/);
    assert.ok(Date.now() - started >= 90, 'child exited before its timer ran');
    console.log('WASIX_PENDING_WAIT_OK');
  } finally {
    clearTimeout(timer);
  }
}

main().catch((error) => {
  console.error(error);
  process.exitCode = 1;
});
