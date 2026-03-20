'use strict';

const assert = require('node:assert/strict');

(async () => {
  let findCacheDir;
  try {
    findCacheDir = require('find-cache-dir');
  } catch (_) {
    const imported = await import('find-cache-dir');
    findCacheDir = imported.default || imported;
  }

  // Call with a name option and verify we get a string path back
  const cacheDir = findCacheDir({ name: 'my-app-cache' });
  // It may return undefined if there's no package.json above cwd,
  // but in a project with node_modules it should return a string
  if (cacheDir !== undefined) {
    assert.equal(typeof cacheDir, 'string', 'should return a string path');
    assert.ok(cacheDir.includes('my-app-cache'), 'path should include the name');
  }

  // Try with a different name to make sure it changes
  const otherDir = findCacheDir({ name: 'other-tool' });
  if (otherDir !== undefined) {
    assert.equal(typeof otherDir, 'string');
    assert.ok(otherDir.includes('other-tool'), 'path should include the other name');
  }

  // If both resolved, they should be different
  if (cacheDir !== undefined && otherDir !== undefined) {
    assert.notEqual(cacheDir, otherDir, 'different names should produce different paths');
  }

  // Verify the create option works (returns the same path type)
  const createdDir = findCacheDir({ name: 'created-cache', create: true });
  if (createdDir !== undefined) {
    assert.equal(typeof createdDir, 'string');
    assert.ok(createdDir.includes('created-cache'));
  }

  console.log('find-cache-dir-test:ok');
})().catch((err) => {
  console.error('find-cache-dir-test:fail');
  console.error(err && err.stack ? err.stack : String(err));
  process.exit(1);
});
