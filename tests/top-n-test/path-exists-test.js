'use strict';

const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');

(async () => {
  const mod = await import('path-exists');
  const { pathExists, pathExistsSync } = mod;

  assert.equal(typeof pathExists, 'function');
  assert.equal(typeof pathExistsSync, 'function');

  // Set up a temp directory with a real file
  const tmpDir = fs.mkdtempSync(path.join(os.tmpdir(), 'topn-pathexists-'));
  const existingFile = path.join(tmpDir, 'exists.txt');
  const missingFile = path.join(tmpDir, 'does-not-exist.txt');
  const existingDir = path.join(tmpDir, 'subdir');

  fs.writeFileSync(existingFile, 'hello');
  fs.mkdirSync(existingDir);

  try {
    // Async: existing file returns true
    assert.equal(await pathExists(existingFile), true);

    // Async: non-existent file returns false
    assert.equal(await pathExists(missingFile), false);

    // Async: existing directory returns true
    assert.equal(await pathExists(existingDir), true);

    // Async: completely bogus path returns false
    assert.equal(await pathExists('/this/path/definitely/does/not/exist'), false);

    // Sync: existing file returns true
    assert.equal(pathExistsSync(existingFile), true);

    // Sync: non-existent file returns false
    assert.equal(pathExistsSync(missingFile), false);

    // Sync: existing directory returns true
    assert.equal(pathExistsSync(existingDir), true);

    // Sync: bogus path returns false
    assert.equal(pathExistsSync('/this/path/definitely/does/not/exist'), false);
  } finally {
    // Clean up
    fs.rmSync(tmpDir, { recursive: true, force: true });
  }

  console.log('path-exists-test:ok');
})().catch((err) => {
  console.error('path-exists-test:fail');
  console.error(err && err.stack ? err.stack : String(err));
  process.exit(1);
});
