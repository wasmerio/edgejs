'use strict';

const assert = require('node:assert/strict');
const hasToStringTag = require('has-tostringtag');

try {
  // hasToStringTag is a function that returns a boolean
  assert.equal(typeof hasToStringTag, 'function');
  const result = hasToStringTag();
  assert.equal(typeof result, 'boolean');

  // The result should be consistent with native Symbol.toStringTag support
  const expected = typeof Symbol === 'function' && typeof Symbol.toStringTag === 'symbol';
  assert.equal(result, expected,
    'hasToStringTag() should match typeof Symbol.toStringTag === "symbol"');

  // In modern Node.js, Symbol.toStringTag is always supported, so it should be true
  assert.equal(result, true, 'Node.js supports Symbol.toStringTag');

  // Calling multiple times returns the same result (it is deterministic)
  assert.equal(hasToStringTag(), hasToStringTag());

  console.log('has-tostringtag-test:ok');
} catch (err) {
  console.error('has-tostringtag-test:fail');
  console.error(err && err.stack ? err.stack : String(err));
  process.exit(1);
}
