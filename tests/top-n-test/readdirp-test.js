'use strict';

const assert = require('node:assert/strict');
const readdirp = require('readdirp');
const path = require('node:path');

(async () => {
  // Read the current project directory recursively, filtering for .js files
  const entries = await readdirp.promise(path.join(__dirname), {
    fileFilter: '*.js',
    depth: 0,
  });

  assert.ok(Array.isArray(entries), 'should return an array');
  assert.ok(entries.length > 0, 'should find at least one .js file');

  // Check that each entry has expected properties
  for (const entry of entries) {
    assert.equal(typeof entry.path, 'string', 'entry should have a path string');
    assert.equal(typeof entry.basename, 'string', 'entry should have a basename');
    assert.ok(entry.basename.endsWith('.js'), 'basename should end with .js');
  }

  // Verify this very test file appears in the results
  const thisFile = entries.find((e) => e.basename === 'readdirp-test.js');
  assert.ok(thisFile, 'should find readdirp-test.js in results');
  assert.equal(thisFile.basename, 'readdirp-test.js');

  // Filter for .json files
  const jsonEntries = await readdirp.promise(path.join(__dirname), {
    fileFilter: '*.json',
    depth: 0,
  });
  assert.ok(Array.isArray(jsonEntries), 'json query should return an array');
  for (const entry of jsonEntries) {
    assert.ok(entry.basename.endsWith('.json'), 'should only include .json files');
  }

  console.log('readdirp-test:ok');
})().catch((err) => {
  console.error('readdirp-test:fail');
  console.error(err && err.stack ? err.stack : String(err));
  process.exit(1);
});
