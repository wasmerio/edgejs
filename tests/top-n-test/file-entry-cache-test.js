'use strict';

const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const os = require('node:os');

try {
  const fileEntryCache = require('file-entry-cache');

  // Set up a temporary directory for the test
  const tmpDir = fs.mkdtempSync(path.join(os.tmpdir(), 'fec-test-'));
  const cacheDir = path.join(tmpDir, '.cache');
  fs.mkdirSync(cacheDir, { recursive: true });

  // Create a test file to track
  const testFile = path.join(tmpDir, 'test.txt');
  fs.writeFileSync(testFile, 'initial content');

  // Create a cache
  const cache = fileEntryCache.create('test-cache', cacheDir);
  assert.ok(cache, 'cache should be created');
  assert.equal(typeof cache.hasFileChanged, 'function');
  assert.equal(typeof cache.reconcile, 'function');
  assert.equal(typeof cache.getFileDescriptor, 'function');

  // First time checking a file should report it as changed
  const changed = cache.hasFileChanged(testFile);
  assert.equal(changed, true, 'new file should be reported as changed');

  // Get file descriptor
  const desc = cache.getFileDescriptor(testFile);
  assert.ok(desc, 'should return a file descriptor');
  assert.equal(desc.changed, true, 'descriptor should indicate changed');
  assert.equal(typeof desc.meta, 'object');

  // Reconcile persists the cache state to disk
  cache.reconcile();

  // After reconcile, reload the cache and check again - should not be changed
  const cache2 = fileEntryCache.create('test-cache', cacheDir);
  const changedAfter = cache2.hasFileChanged(testFile);
  assert.equal(changedAfter, false, 'file should not be changed after reconcile');

  // Modify the file and check again
  fs.writeFileSync(testFile, 'modified content');
  const changedAgain = cache2.hasFileChanged(testFile);
  assert.equal(changedAgain, true, 'modified file should be reported as changed');

  // Reconcile again and verify
  cache2.reconcile();
  const cache3 = fileEntryCache.create('test-cache', cacheDir);
  assert.equal(cache3.hasFileChanged(testFile), false, 'should be stable after second reconcile');

  // Clean up temp files
  fs.rmSync(tmpDir, { recursive: true, force: true });

  console.log('file-entry-cache-test:ok');
} catch (err) {
  console.error('file-entry-cache-test:fail');
  console.error(err && err.stack ? err.stack : String(err));
  process.exit(1);
}
