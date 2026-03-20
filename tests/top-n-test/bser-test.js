'use strict';

const assert = require('node:assert/strict');

const bser = require('bser');

// Encode a simple object
const original = { name: 'test', values: [1, 2, 3], flag: true };
const encoded = bser.dumpToBuffer(original);
assert.ok(Buffer.isBuffer(encoded), 'encoded result should be a Buffer');
assert.ok(encoded.length > 0, 'encoded buffer should not be empty');

// Decode it back and verify round-trip
const decoded = bser.loadFromBuffer(encoded);
assert.deepEqual(decoded, original, 'decoded value should match the original');

// Test with an array at top level
const arr = [10, 'hello', null, true];
const encodedArr = bser.dumpToBuffer(arr);
const decodedArr = bser.loadFromBuffer(encodedArr);
assert.deepEqual(decodedArr, arr, 'array round-trip should work');

// Test with a nested structure
const nested = { outer: { inner: [1, 2, { deep: 'value' }] } };
const encodedNested = bser.dumpToBuffer(nested);
const decodedNested = bser.loadFromBuffer(encodedNested);
assert.deepEqual(decodedNested, nested, 'nested structure round-trip should work');

// Test with a simple string
const str = 'just a string';
const encodedStr = bser.dumpToBuffer(str);
const decodedStr = bser.loadFromBuffer(encodedStr);
assert.equal(decodedStr, str, 'string round-trip should work');

console.log('bser-test:ok');
