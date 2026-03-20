'use strict';

const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');

(async () => {
  const walker = require('walker');

  // Create a temp directory structure to walk
  const root = fs.mkdtempSync(path.join(os.tmpdir(), 'walker-test-'));
  const subdir = path.join(root, 'sub');
  fs.mkdirSync(subdir);
  fs.writeFileSync(path.join(root, 'hello.txt'), 'hello');
  fs.writeFileSync(path.join(root, 'world.txt'), 'world');
  fs.writeFileSync(path.join(subdir, 'nested.txt'), 'nested');

  // Walk the directory and collect file paths
  const files = [];
  const dirs = [];
  await new Promise((resolve, reject) => {
    walker(root)
      .on('file', (filePath) => {
        files.push(path.relative(root, filePath));
      })
      .on('dir', (dirPath) => {
        dirs.push(path.relative(root, dirPath));
      })
      .on('error', reject)
      .on('end', resolve);
  });

  // Verify we found all the files
  files.sort();
  assert.deepEqual(files, ['hello.txt', path.join('sub', 'nested.txt'), 'world.txt']);

  // Verify directory events fired (root shows as empty string from relative)
  assert.ok(dirs.length > 0, 'should have seen at least one directory');

  // Verify walker returns an EventEmitter-like object
  const instance = walker(root);
  assert.equal(typeof instance.on, 'function', 'walker instance should have .on method');
  // Wait for it to finish so we don't leave dangling handles
  await new Promise((resolve) => instance.on('end', resolve));

  // Cleanup
  fs.rmSync(root, { recursive: true, force: true });

  console.log('walker-test:ok');
})().catch((err) => {
  console.error('walker-test:fail');
  console.error(err && err.stack ? err.stack : String(err));
  process.exit(1);
});
