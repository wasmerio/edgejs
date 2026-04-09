'use strict';

const assert = require('node:assert/strict');

const merge = require('lodash.merge');

// Basic deep merge of nested objects
const target = { a: 1, nested: { x: 10, y: 20 } };
const source = { b: 2, nested: { y: 99, z: 30 } };
const result = merge({}, target, source);

assert.equal(result.a, 1, 'should keep properties from target');
assert.equal(result.b, 2, 'should add properties from source');
assert.equal(result.nested.x, 10, 'should keep nested props not in source');
assert.equal(result.nested.y, 99, 'should overwrite nested props from source');
assert.equal(result.nested.z, 30, 'should add new nested props from source');

// Merge mutates the first argument
const obj = { foo: 'bar' };
merge(obj, { baz: 'qux' });
assert.equal(obj.baz, 'qux', 'merge should mutate the destination object');

// Merge arrays (lodash.merge merges arrays by index, not concatenation)
const arrResult = merge({}, { items: [1, 2, 3] }, { items: [10] });
assert.equal(arrResult.items[0], 10, 'should merge array at index 0');
assert.equal(arrResult.items[1], 2, 'should keep array value at index 1');
assert.equal(arrResult.items[2], 3, 'should keep array value at index 2');

// Merge with multiple sources
const multi = merge({}, { a: 1 }, { b: 2 }, { c: 3 });
assert.deepEqual(multi, { a: 1, b: 2, c: 3 }, 'should merge multiple sources');

// Undefined values in source should not overwrite existing values
const withUndef = merge({}, { key: 'value' }, { key: undefined });
assert.equal(withUndef.key, 'value', 'undefined should not overwrite existing value');

// Deep nesting several levels
const deep = merge(
  {},
  { level1: { level2: { level3: { val: 'original' } } } },
  { level1: { level2: { level3: { extra: 'added' } } } }
);
assert.equal(deep.level1.level2.level3.val, 'original');
assert.equal(deep.level1.level2.level3.extra, 'added');

console.log('lodash.merge-test:ok');
