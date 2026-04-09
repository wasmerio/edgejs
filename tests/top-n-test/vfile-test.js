'use strict';

const assert = require('node:assert/strict');

(async () => {
  const { VFile } = await import('vfile');

  // Create a VFile with path and contents
  const file = new VFile({
    path: 'src/hello.md',
    value: '# Hello World\n\nSome content here.',
  });

  // Check basic properties
  assert.equal(file.path, 'src/hello.md');
  assert.equal(file.value, '# Hello World\n\nSome content here.');
  assert.equal(file.basename, 'hello.md');
  assert.equal(file.extname, '.md');
  assert.equal(file.stem, 'hello');
  assert.equal(file.dirname, 'src');

  // toString returns the contents
  assert.equal(String(file), '# Hello World\n\nSome content here.');

  // Add a warning message
  const msg = file.message('Heading should use sentence case', { line: 1, column: 1 });
  assert.equal(file.messages.length, 1);
  assert.ok(msg.message.includes('sentence case'));
  assert.equal(msg.fatal, false);

  // Create a simple VFile with just string content
  const simple = new VFile('plain text content');
  assert.equal(simple.value, 'plain text content');
  assert.equal(String(simple), 'plain text content');

  // Empty VFile
  const empty = new VFile();
  assert.equal(empty.value, undefined);
  assert.deepEqual(empty.messages, []);

  console.log('vfile-test:ok');
})().catch((err) => {
  console.error('vfile-test:fail');
  console.error(err && err.stack ? err.stack : String(err));
  process.exit(1);
});
