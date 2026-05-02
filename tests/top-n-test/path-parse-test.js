'use strict';

const assert = require('node:assert/strict');

try {
  const pathParse = require('path-parse');

  // Parse an absolute Unix-style path
  const abs = pathParse('/home/user/docs/report.pdf');
  assert.equal(abs.root, '/');
  assert.equal(abs.dir, '/home/user/docs');
  assert.equal(abs.base, 'report.pdf');
  assert.equal(abs.ext, '.pdf');
  assert.equal(abs.name, 'report');

  // Parse a relative path
  const rel = pathParse('src/utils/helpers.js');
  assert.equal(rel.root, '');
  assert.equal(rel.dir, 'src/utils');
  assert.equal(rel.base, 'helpers.js');
  assert.equal(rel.ext, '.js');
  assert.equal(rel.name, 'helpers');

  // Parse a file with no extension
  const noExt = pathParse('/etc/hostname');
  assert.equal(noExt.root, '/');
  assert.equal(noExt.dir, '/etc');
  assert.equal(noExt.base, 'hostname');
  assert.equal(noExt.ext, '');
  assert.equal(noExt.name, 'hostname');

  // Parse a dotfile
  const dotfile = pathParse('/home/user/.bashrc');
  assert.equal(dotfile.root, '/');
  assert.equal(dotfile.base, '.bashrc');
  assert.equal(dotfile.name, '.bashrc');
  assert.equal(dotfile.ext, '');

  // Parse a file with multiple dots
  const multiDot = pathParse('/tmp/archive.tar.gz');
  assert.equal(multiDot.base, 'archive.tar.gz');
  assert.equal(multiDot.ext, '.gz');
  assert.equal(multiDot.name, 'archive.tar');

  // Parse just a filename
  const justFile = pathParse('index.html');
  assert.equal(justFile.root, '');
  assert.equal(justFile.dir, '');
  assert.equal(justFile.base, 'index.html');
  assert.equal(justFile.ext, '.html');
  assert.equal(justFile.name, 'index');

  // Result should have exactly the five expected properties
  const keys = Object.keys(abs).sort();
  assert.deepEqual(keys, ['base', 'dir', 'ext', 'name', 'root']);

  console.log('path-parse-test:ok');
} catch (err) {
  console.error('path-parse-test:fail');
  console.error(err && err.stack ? err.stack : String(err));
  process.exit(1);
}
