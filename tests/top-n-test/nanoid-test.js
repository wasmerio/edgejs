'use strict';

const assert = require('node:assert/strict');

(async () => {
  const { nanoid, customAlphabet } = await import('nanoid');

  // Generate a default ID (21 chars by default)
  const id1 = nanoid();
  assert.equal(typeof id1, 'string');
  assert.equal(id1.length, 21);

  // Generate another and verify uniqueness
  const id2 = nanoid();
  assert.notEqual(id1, id2, 'two generated IDs should be different');

  // Generate with custom size
  const short = nanoid(10);
  assert.equal(short.length, 10);

  const long = nanoid(50);
  assert.equal(long.length, 50);

  // Verify bulk uniqueness
  const ids = new Set();
  for (let i = 0; i < 1000; i++) {
    ids.add(nanoid());
  }
  assert.equal(ids.size, 1000, 'all 1000 IDs should be unique');

  // Custom alphabet
  const hexId = customAlphabet('0123456789abcdef', 16);
  const hex = hexId();
  assert.equal(hex.length, 16);
  assert.ok(/^[0-9a-f]+$/.test(hex), 'custom alphabet ID should only contain hex chars');

  // Custom alphabet with digits only
  const numericId = customAlphabet('0123456789', 8);
  const num = numericId();
  assert.equal(num.length, 8);
  assert.ok(/^\d+$/.test(num), 'numeric ID should only contain digits');

  console.log('nanoid-test:ok');
})().catch((err) => {
  console.error('nanoid-test:fail');
  console.error(err && err.stack ? err.stack : String(err));
  process.exit(1);
});
