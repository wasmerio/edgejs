'use strict';

const assert = require('node:assert/strict');
const encodeUrl = require('encodeurl');

try {
  // Encode URL with spaces
  assert.equal(encodeUrl('/foo bar'), '/foo%20bar');

  // Already-encoded characters should stay encoded
  assert.equal(encodeUrl('/already%20encoded'), '/already%20encoded');

  // Encode a full URL with spaces in path and query preserved
  assert.equal(
    encodeUrl('http://example.com/a b?x=1&y=2'),
    'http://example.com/a%20b?x=1&y=2'
  );

  // Path characters like / : @ should not be encoded
  const preservedChars = encodeUrl('/path/to:resource@host');
  assert.equal(preservedChars, '/path/to:resource@host');

  // Parentheses and other unencoded chars should stay as-is
  assert.equal(encodeUrl('/foo(bar)'), '/foo(bar)');

  // Encode unicode characters
  const unicodeResult = encodeUrl('/caf\u00e9');
  assert.ok(unicodeResult.includes('%'), 'unicode should be percent-encoded');
  assert.ok(!unicodeResult.includes('\u00e9'), 'raw unicode char should be encoded');

  // Empty string stays empty
  assert.equal(encodeUrl(''), '');

  // Hash fragment characters preserved
  assert.equal(encodeUrl('/path#fragment'), '/path#fragment');

  console.log('encodeurl-test:ok');
} catch (err) {
  console.error('encodeurl-test:fail');
  console.error(err && err.stack ? err.stack : String(err));
  process.exit(1);
}
