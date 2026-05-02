'use strict';

const assert = require('node:assert/strict');

(async () => {
  const which = require('which');

  // Find 'node' - it must be on PATH since we're running in node
  const nodePath = await which('node');
  assert.ok(typeof nodePath === 'string');
  assert.ok(nodePath.length > 0);
  assert.ok(nodePath.includes('node'), 'resolved path should contain "node"');

  // Sync version also works
  const nodePathSync = which.sync('node');
  assert.ok(typeof nodePathSync === 'string');
  assert.ok(nodePathSync.length > 0);

  // Find 'ls' or another common executable
  const lsPath = which.sync('ls');
  assert.ok(typeof lsPath === 'string');
  assert.ok(lsPath.length > 0);

  // Handle not-found: async rejects
  await assert.rejects(
    () => which('this-executable-surely-does-not-exist-xyz'),
    (err) => {
      assert.ok(err instanceof Error);
      assert.equal(err.code, 'ENOENT');
      return true;
    }
  );

  // Handle not-found: sync throws
  assert.throws(
    () => which.sync('this-executable-surely-does-not-exist-xyz'),
    (err) => {
      assert.ok(err instanceof Error);
      assert.equal(err.code, 'ENOENT');
      return true;
    }
  );

  // nothrow option returns null instead of throwing
  const notFound = which.sync('this-executable-surely-does-not-exist-xyz', { nothrow: true });
  assert.equal(notFound, null);

  console.log('which-test:ok');
})().catch((err) => {
  console.error('which-test:fail');
  console.error(err && err.stack ? err.stack : String(err));
  process.exit(1);
});
