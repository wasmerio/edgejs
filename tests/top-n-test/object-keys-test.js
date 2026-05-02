'use strict';

const assert = require('node:assert/strict');

const objectKeys = require('object-keys');

// Basic usage: get keys of a plain object
const keys = objectKeys({ b: 2, a: 1, c: 3 });
assert.ok(Array.isArray(keys), 'should return an array');
assert.equal(keys.length, 3, 'should return all own keys');
assert.ok(keys.includes('a'), 'should include key "a"');
assert.ok(keys.includes('b'), 'should include key "b"');
assert.ok(keys.includes('c'), 'should include key "c"');

// Empty object
assert.deepEqual(objectKeys({}), [], 'empty object should return empty array');

// Should not include inherited prototype properties
const proto = { inherited: true };
const obj = Object.create(proto);
obj.own = 'yes';
const ownKeys = objectKeys(obj);
assert.deepEqual(ownKeys, ['own'], 'should only return own properties, not inherited');

// String argument returns character indices
const strKeys = objectKeys('abc');
assert.deepEqual(strKeys, ['0', '1', '2'], 'string should return index keys');

// Object with non-enumerable property
const withNonEnum = {};
Object.defineProperty(withNonEnum, 'hidden', { value: 42, enumerable: false });
withNonEnum.visible = true;
const visibleKeys = objectKeys(withNonEnum);
assert.deepEqual(visibleKeys, ['visible'], 'should not include non-enumerable keys');

// Verify it returns keys in insertion order for simple objects
const ordered = {};
ordered.first = 1;
ordered.second = 2;
ordered.third = 3;
const orderedKeys = objectKeys(ordered);
assert.deepEqual(orderedKeys, ['first', 'second', 'third'], 'should preserve insertion order');

console.log('object-keys-test:ok');
