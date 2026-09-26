'use strict';

const assert = require('node:assert/strict');
const { spawn } = require('node:child_process');
const { once } = require('node:events');

async function main() {
  assert.equal(process.kill(process.pid, 0), true);
  assert.throws(() => process.kill(2147483647, 0), { code: 'ESRCH' });
  assert.throws(() => process.kill(-2147483647, 0), { code: 'ESRCH' });

  const child = spawn(process.execPath, ['-e',
    'setInterval(() => {}, 1000); require("fs").writeSync(1, "ready")',
  ], { detached: true });
  const exited = once(child, 'exit');
  const closed = once(child, 'close');
  try {
    await once(child.stdout, 'data');
    assert.equal(process.kill(child.pid, 0), true);
    // Match Pi's group-kill attempt and immediate-child fallback. WASIX
    // accepts detached spawning but does not create the requested group.
    if (process.platform === 'wasi') {
      assert.throws(() => process.kill(-child.pid, 'SIGKILL'), { code: 'ESRCH' });
    }
  } finally {
    process.kill(child.pid, 'SIGKILL');
    await exited;
    // A forcibly terminated SDK worker may leave its pipe writers open.
    // Match callers such as Pi that close their streams after child exit.
    child.stdin.destroy();
    child.stdout.destroy();
    child.stderr.destroy();
    await closed;
  }
  console.log('WASIX_PROCESS_KILL_OK');
}

main().catch((error) => {
  console.error(error);
  process.exitCode = 1;
});
