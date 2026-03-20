'use strict';

const assert = require('node:assert/strict');

(async () => {
  const mod = await import('strip-bom');
  const stripBom = mod.default;

  // The UTF-8 BOM character
  const bom = '\uFEFF';

  // Strip BOM from a string that starts with one
  const withBom = bom + 'hello world';
  assert.equal(stripBom(withBom), 'hello world');

  // A string without BOM should remain unchanged
  const noBom = 'hello world';
  assert.equal(stripBom(noBom), 'hello world');

  // Verify that the BOM character is actually present before stripping
  assert.equal(withBom.charCodeAt(0), 0xFEFF, 'first char should be BOM');
  assert.equal(withBom.length, 'hello world'.length + 1);

  // After stripping, the first char is 'h', not BOM
  const stripped = stripBom(withBom);
  assert.equal(stripped.charCodeAt(0), 'h'.charCodeAt(0));
  assert.equal(stripped.length, 'hello world'.length);

  // Empty string should pass through
  assert.equal(stripBom(''), '');

  // BOM only should become empty string
  assert.equal(stripBom(bom), '');

  console.log('strip-bom-test:ok');
})().catch((err) => {
  console.error('strip-bom-test:fail');
  console.error(err && err.stack ? err.stack : String(err));
  process.exit(1);
});
