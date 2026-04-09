'use strict';

const assert = require('node:assert/strict');

const {
  stringify,
  printReceived,
  printExpected,
  matcherHint,
  diff,
} = require('jest-matcher-utils');

// stringify should convert values to readable strings
assert.equal(typeof stringify(42), 'string', 'stringify should return a string');
assert.ok(stringify(42).includes('42'), 'stringify(42) should include "42"');
assert.ok(stringify('hello').includes('hello'), 'stringify("hello") should include "hello"');
assert.ok(stringify(null).includes('null'), 'stringify(null) should include "null"');
assert.ok(stringify(undefined).includes('undefined'), 'stringify(undefined) should include "undefined"');
assert.ok(stringify({ a: 1 }).includes('a'), 'stringify of object should include key');

// printReceived wraps value for display (typically in red)
const received = printReceived('actual');
assert.equal(typeof received, 'string', 'printReceived should return a string');
assert.ok(received.includes('actual'), 'printReceived should include the value');

// printExpected wraps value for display (typically in green)
const expected = printExpected('expected');
assert.equal(typeof expected, 'string', 'printExpected should return a string');
assert.ok(expected.includes('expected'), 'printExpected should include the value');

// matcherHint produces a formatted hint string
const hint = matcherHint('.toBe');
assert.equal(typeof hint, 'string', 'matcherHint should return a string');
assert.ok(hint.includes('toBe'), 'matcherHint should include the matcher name');

// matcherHint with custom arguments
const hintCustom = matcherHint('.toEqual', 'received', 'expected');
assert.ok(hintCustom.includes('toEqual'));

// diff shows differences between two values
const diffResult = diff({ a: 1, b: 2 }, { a: 1, b: 3 });
// diff can return null or a string depending on values
if (diffResult !== null) {
  assert.equal(typeof diffResult, 'string', 'diff should return a string when values differ');
}

// diff with identical values
const noDiff = diff('same', 'same');
// When values are identical, diff may return a "no visual difference" message or null
if (noDiff !== null) {
  assert.equal(typeof noDiff, 'string');
}

// diff with different types
const typeDiff = diff(42, 'forty-two');
if (typeDiff !== null) {
  assert.equal(typeof typeDiff, 'string');
}

console.log('jest-matcher-utils-test:ok');
