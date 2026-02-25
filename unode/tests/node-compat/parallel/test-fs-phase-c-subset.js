'use strict';
require('../common');
const assert = require('assert');
const fs = require('fs');
const path = require('path');
const tmpdir = require('../common/tmpdir');

tmpdir.refresh();
const dir = tmpdir.path;

// truncateSync / ftruncateSync
const truncFile = path.join(dir, 'truncate-file.txt');
fs.writeFileSync(truncFile, 'hello world', 'utf8');
fs.truncateSync(truncFile, 5);
assert.strictEqual(fs.readFileSync(truncFile, 'utf8'), 'hello');
fs.unlinkSync(truncFile);

// renameSync
const oldPath = path.join(dir, 'old-name.txt');
const newPath = path.join(dir, 'new-name.txt');
fs.writeFileSync(oldPath, 'content', 'utf8');
fs.renameSync(oldPath, newPath);
assert.strictEqual(fs.existsSync(oldPath), false);
assert.strictEqual(fs.existsSync(newPath), true);
assert.strictEqual(fs.readFileSync(newPath, 'utf8'), 'content');

// unlinkSync
fs.unlinkSync(newPath);
assert.strictEqual(fs.existsSync(newPath), false);

// copyFileSync
const srcFile = path.join(dir, 'copy-src.txt');
const destFile = path.join(dir, 'copy-dest.txt');
fs.writeFileSync(srcFile, 'copied content', 'utf8');
fs.copyFileSync(srcFile, destFile);
assert.strictEqual(fs.readFileSync(destFile, 'utf8'), 'copied content');
fs.unlinkSync(srcFile);
fs.unlinkSync(destFile);

// appendFileSync (string)
const appendFile = path.join(dir, 'append.txt');
fs.writeFileSync(appendFile, 'AB', 'utf8');
fs.appendFileSync(appendFile, 'CD');
assert.strictEqual(fs.readFileSync(appendFile, 'utf8'), 'ABCD');
fs.unlinkSync(appendFile);

// appendFileSync (Buffer / Uint8Array)
const appendFile2 = path.join(dir, 'append2.txt');
fs.writeFileSync(appendFile2, 'XY', 'utf8');
const buf = Buffer.from ? Buffer.from('Z', 'utf8') : new Uint8Array([0x5a]);
fs.appendFileSync(appendFile2, buf);
const read = fs.readFileSync(appendFile2);
const len = read && (read.length !== undefined ? read.length : (read.byteLength || 0));
assert.strictEqual(len, 3, 'append with buffer should give 3 bytes');
fs.unlinkSync(appendFile2);

// rmdirSync (non-recursive)
const subDir = path.join(dir, 'subdir');
fs.mkdirSync(subDir, { recursive: true });
fs.rmdirSync(subDir);
assert.strictEqual(fs.existsSync(subDir), false);
