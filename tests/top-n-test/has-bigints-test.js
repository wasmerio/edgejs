'use strict';

const assert = require('node:assert/strict');

const hasBigInts = require('has-bigints');

// Returns a boolean
const result = hasBigInts();
assert.equal(typeof result, 'boolean');

// Should match the runtime's actual BigInt support
const expected = typeof BigInt !== 'undefined';
assert.equal(result, expected);

// If BigInt is supported, verify it works
if (result) {
  assert.equal(typeof BigInt(0), 'bigint');
}

console.log('has-bigints-test:ok');
