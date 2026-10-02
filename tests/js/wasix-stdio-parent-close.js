'use strict';

const assert = require('node:assert/strict');
const { spawn } = require('node:child_process');
const fs = require('node:fs');
const { once } = require('node:events');

async function main() {
  const target = 64;
  const held = [];
  let child;
  let watchdog;
  try {
    // Force the parent end of the next pipe to share the child's target fd.
    // Inherit ordinary stdio so it does not allocate intervening pipes.
    for (;;) {
      const fd = fs.openSync('/dev/null', 'r');
      held.push(fd);
      assert(fd <= target, 'target descriptor was already occupied');
      if (fd === target) break;
    }
    fs.closeSync(held.pop());
    const stdio = Array(target + 1).fill('ignore');
    stdio[0] = stdio[1] = stdio[2] = 'inherit';
    stdio[target] = 'pipe';
    child = spawn(process.execPath, ['-e',
      `require('node:fs').writeSync(${target}, 'child stdio survives\\n')`,
    ], { stdio });
    const closed = once(child, 'close');
    // Observe rejection immediately if setup or a stream assertion fails.
    closed.catch(() => {});
    assert.equal(child.stdio[target]._handle.fd, target);
    let output = '';
    child.stdio[target].on('data', (chunk) => { output += chunk; });
    child.stdio[target].on('error', (error) => { throw error; });
    watchdog = setTimeout(() => {
      console.error('child stdio regression timed out');
      process.exit(1);
    }, 15000);
    const [code, signal] = await closed;
    assert.equal(code, 0);
    assert.equal(signal, null);
    assert.equal(output, 'child stdio survives\n');
  } finally {
    clearTimeout(watchdog);
    for (const fd of held) fs.closeSync(fd);
    if (child && child.exitCode === null) child.kill('SIGKILL');
    if (child) child.stdio[target]?.destroy();
  }
  console.log('WASIX_STDIO_PARENT_CLOSE_OK');
}

main().catch((error) => {
  console.error(error);
  process.exitCode = 1;
});
