'use strict';

const assert = require('node:assert/strict');
const jsonfile = require('jsonfile');
const path = require('node:path');
const os = require('node:os');
const fs = require('node:fs');

(async () => {
  const tmpDir = os.tmpdir();
  const testFile = path.join(tmpDir, 'jsonfile-test-' + Date.now() + '.json');

  // Write JSON to a temp file and read it back
  const data = { name: 'Alice', age: 30, hobbies: ['reading', 'coding'] };
  await jsonfile.writeFile(testFile, data);
  const result = await jsonfile.readFile(testFile);
  assert.deepEqual(result, data);

  // Write with spaces option for pretty printing
  const prettyFile = path.join(tmpDir, 'jsonfile-test-pretty-' + Date.now() + '.json');
  await jsonfile.writeFile(prettyFile, { hello: 'world' }, { spaces: 2 });
  const prettyContent = fs.readFileSync(prettyFile, 'utf8');
  assert.ok(prettyContent.includes('\n'), 'pretty-printed JSON should have newlines');
  assert.ok(prettyContent.includes('  '), 'pretty-printed JSON should have 2-space indent');
  const prettyResult = await jsonfile.readFile(prettyFile);
  assert.equal(prettyResult.hello, 'world');

  // Write and read sync variants
  const syncFile = path.join(tmpDir, 'jsonfile-test-sync-' + Date.now() + '.json');
  jsonfile.writeFileSync(syncFile, { sync: true, count: 99 });
  const syncResult = jsonfile.readFileSync(syncFile);
  assert.equal(syncResult.sync, true);
  assert.equal(syncResult.count, 99);

  // Error handling: reading a non-existent file should reject
  try {
    await jsonfile.readFile('/tmp/this-file-does-not-exist-xyz.json');
    assert.fail('should have thrown for non-existent file');
  } catch (err) {
    assert.ok(err.code === 'ENOENT', 'error code should be ENOENT');
  }

  // Clean up
  fs.unlinkSync(testFile);
  fs.unlinkSync(prettyFile);
  fs.unlinkSync(syncFile);

  console.log('jsonfile-test:ok');
})().catch((err) => {
  console.error('jsonfile-test:fail');
  console.error(err && err.stack ? err.stack : String(err));
  process.exit(1);
});
