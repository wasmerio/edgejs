'use strict';

const assert = require('node:assert/strict');
const escalade = require('escalade');
const path = require('node:path');

(async () => {
  // Find package.json starting from this directory, going up
  const pkgPath = await escalade(__dirname, (dir, names) => {
    if (names.includes('package.json')) return 'package.json';
  });
  assert.equal(typeof pkgPath, 'string');
  assert.ok(pkgPath.endsWith('package.json'), 'should find package.json');

  // Find node_modules directory going up from this directory
  const nmPath = await escalade(__dirname, (dir, names) => {
    if (names.includes('node_modules')) return 'node_modules';
  });
  assert.equal(typeof nmPath, 'string');
  assert.ok(nmPath.endsWith('node_modules'), 'should find node_modules');

  // Searching for a non-existent file should return undefined
  const notFound = await escalade(__dirname, (dir, names) => {
    if (names.includes('this-file-does-not-exist-xyz.txt')) {
      return 'this-file-does-not-exist-xyz.txt';
    }
  });
  assert.equal(notFound, undefined, 'non-existent file should return undefined');

  // Starting from a deeper nested directory should still find project root files
  const nodeModulesDir = path.join(__dirname, 'node_modules');
  const foundFromDeep = await escalade(nodeModulesDir, (dir, names) => {
    if (names.includes('package.json')) return 'package.json';
  });
  assert.equal(typeof foundFromDeep, 'string');
  assert.ok(foundFromDeep.endsWith('package.json'));

  console.log('escalade-test:ok');
})().catch((err) => {
  console.error('escalade-test:fail');
  console.error(err && err.stack ? err.stack : String(err));
  process.exit(1);
});
