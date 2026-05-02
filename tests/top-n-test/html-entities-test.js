'use strict';

const assert = require('node:assert/strict');
const { encode, decode } = require('html-entities');

try {
  // Encode special HTML characters
  const encoded = encode('Tom & Jerry <script>"alert"</script>');
  assert.ok(encoded.includes('&amp;'), 'should encode ampersand');
  assert.ok(encoded.includes('&lt;'), 'should encode less-than');
  assert.ok(encoded.includes('&gt;'), 'should encode greater-than');
  assert.ok(encoded.includes('&quot;'), 'should encode double quote');

  // Decode them back
  const decoded = decode('Tom &amp; Jerry &lt;script&gt;&quot;alert&quot;&lt;/script&gt;');
  assert.equal(decoded, 'Tom & Jerry <script>"alert"</script>');

  // Named entities
  assert.equal(decode('&copy;'), '\u00A9');
  assert.equal(decode('&nbsp;'), '\u00A0');

  // Numeric entities (decimal and hex)
  assert.equal(decode('&#65;'), 'A');
  assert.equal(decode('&#x41;'), 'A');
  assert.equal(decode('&#169;'), '\u00A9');

  // Round-trip: encode then decode should give back the original
  const original = 'Price: $5 < $10 & "free" > nothing';
  assert.equal(decode(encode(original)), original);

  // Plain text without special chars should pass through unchanged
  assert.equal(encode('hello world'), 'hello world');
  assert.equal(decode('hello world'), 'hello world');

  console.log('html-entities-test:ok');
} catch (err) {
  console.error('html-entities-test:fail');
  console.error(err && err.stack ? err.stack : String(err));
  process.exit(1);
}
