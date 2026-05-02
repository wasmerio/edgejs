'use strict';

const assert = require('node:assert/strict');

// core-js is a polyfill library -- importing it patches globals
require('core-js');

// Array.from works
const arr = Array.from('abc');
assert.deepEqual(arr, ['a', 'b', 'c']);
assert.deepEqual(Array.from([1, 2, 3], (x) => x * 2), [2, 4, 6]);

// Object.assign works
const target = { a: 1 };
const result = Object.assign(target, { b: 2 }, { c: 3 });
assert.equal(result, target);
assert.deepEqual(result, { a: 1, b: 2, c: 3 });

// Promise exists and works
assert.equal(typeof Promise, 'function');
const p = Promise.resolve(42);
assert.ok(p instanceof Promise);

// Symbol exists
assert.equal(typeof Symbol, 'function');
const sym = Symbol('test');
assert.equal(typeof sym, 'symbol');

// Map works
const map = new Map();
map.set('key', 'value');
assert.equal(map.get('key'), 'value');
assert.equal(map.size, 1);
assert.equal(map.has('key'), true);

// Set works
const set = new Set([1, 2, 3, 2, 1]);
assert.equal(set.size, 3);
assert.equal(set.has(1), true);
assert.equal(set.has(4), false);

// Array.prototype.includes works
assert.equal([1, 2, 3].includes(2), true);
assert.equal([1, 2, 3].includes(4), false);

console.log('core-js-test:ok');
