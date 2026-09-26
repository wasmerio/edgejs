'use strict';

const assert = require('node:assert/strict');
const { spawn, spawnSync } = require('node:child_process');
const { once } = require('node:events');

async function main() {
  const script = 'const fs = require("fs"); fs.writeSync(1, "detached stdout"); fs.writeSync(2, "detached stderr"); process.exitCode = 7';
  const sync = spawnSync(process.execPath, ['-e', script], {
    detached: true,
    encoding: 'utf8',
  });
  assert.ifError(sync.error);
  assert.equal(sync.status, 7);
  assert.equal(sync.stdout, 'detached stdout');
  assert.equal(sync.stderr, 'detached stderr');

  const child = spawn(process.execPath, ['-e', script], { detached: true });
  let stdout = '';
  let stderr = '';
  child.stdout.on('data', (chunk) => { stdout += chunk; });
  child.stderr.on('data', (chunk) => { stderr += chunk; });
  const [code, signal] = await once(child, 'close');
  assert.equal(code, 7);
  assert.equal(signal, null);
  assert.equal(stdout, 'detached stdout');
  assert.equal(stderr, 'detached stderr');

  const missing = spawnSync('/missing-edge-spawn-test', [], { detached: true });
  // Older WASIX hosts report ENOEXEC for a missing executable.
  assert(['ENOENT', 'ENOEXEC'].includes(missing.error.code));
  console.log('WASIX_DETACHED_SPAWN_OK');
}

main().catch((error) => {
  console.error(error);
  process.exitCode = 1;
});
