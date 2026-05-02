'use strict';

const assert = require('node:assert/strict');
const path = require('node:path');
const os = require('node:os');
const fs = require('node:fs');

(async () => {
  const fse = require('fs-extra');

  // Create a unique temp directory for our tests
  const tmpBase = fs.mkdtempSync(path.join(os.tmpdir(), 'topn-fse-'));

  try {
    // ensureDir creates nested directories
    const nestedDir = path.join(tmpBase, 'a', 'b', 'c');
    await fse.ensureDir(nestedDir);
    assert.ok(fs.existsSync(nestedDir), 'nested directories should exist');

    // ensureDirSync works too
    const nestedDir2 = path.join(tmpBase, 'x', 'y', 'z');
    fse.ensureDirSync(nestedDir2);
    assert.ok(fs.existsSync(nestedDir2), 'sync nested directories should exist');

    // writeJson and readJson
    const jsonFile = path.join(tmpBase, 'data.json');
    const data = { name: 'test', items: [1, 2, 3], nested: { ok: true } };
    await fse.writeJson(jsonFile, data, { spaces: 2 });
    assert.ok(fs.existsSync(jsonFile), 'JSON file should exist');

    const readBack = await fse.readJson(jsonFile);
    assert.deepEqual(readBack, data);

    // writeJsonSync / readJsonSync
    const jsonFile2 = path.join(tmpBase, 'data2.json');
    fse.writeJsonSync(jsonFile2, { hello: 'world' });
    const readBack2 = fse.readJsonSync(jsonFile2);
    assert.deepEqual(readBack2, { hello: 'world' });

    // copy a file
    const srcFile = path.join(tmpBase, 'source.txt');
    const destFile = path.join(tmpBase, 'dest.txt');
    fs.writeFileSync(srcFile, 'copy me');
    await fse.copy(srcFile, destFile);
    assert.equal(fs.readFileSync(destFile, 'utf8'), 'copy me');

    // copy a directory
    const srcDir = path.join(tmpBase, 'srcdir');
    const destDir = path.join(tmpBase, 'destdir');
    await fse.ensureDir(srcDir);
    fs.writeFileSync(path.join(srcDir, 'file.txt'), 'in dir');
    await fse.copy(srcDir, destDir);
    assert.equal(fs.readFileSync(path.join(destDir, 'file.txt'), 'utf8'), 'in dir');

    // pathExists
    assert.equal(await fse.pathExists(srcFile), true);
    assert.equal(await fse.pathExists(path.join(tmpBase, 'nope.txt')), false);

    // pathExistsSync
    assert.equal(fse.pathExistsSync(srcFile), true);
    assert.equal(fse.pathExistsSync(path.join(tmpBase, 'nope.txt')), false);

    // remove
    const toRemove = path.join(tmpBase, 'removeme');
    await fse.ensureDir(toRemove);
    fs.writeFileSync(path.join(toRemove, 'inner.txt'), 'bye');
    assert.ok(fs.existsSync(toRemove));
    await fse.remove(toRemove);
    assert.ok(!fs.existsSync(toRemove), 'directory should be removed');

    // ensureFile creates file and parent directories
    const deepFile = path.join(tmpBase, 'deep', 'path', 'file.txt');
    await fse.ensureFile(deepFile);
    assert.ok(fs.existsSync(deepFile), 'ensureFile should create the file');

    // move
    const moveFrom = path.join(tmpBase, 'movefrom.txt');
    const moveTo = path.join(tmpBase, 'moveto.txt');
    fs.writeFileSync(moveFrom, 'moving');
    await fse.move(moveFrom, moveTo);
    assert.ok(!fs.existsSync(moveFrom), 'source should not exist after move');
    assert.equal(fs.readFileSync(moveTo, 'utf8'), 'moving');

  } finally {
    // Clean up
    await fse.remove(tmpBase);
  }

  console.log('fs-extra-test:ok');
})().catch((err) => {
  console.error('fs-extra-test:fail');
  console.error(err && err.stack ? err.stack : String(err));
  process.exit(1);
});
