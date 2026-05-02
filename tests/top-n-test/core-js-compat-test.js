'use strict';

const assert = require('node:assert/strict');

const compat = require('core-js-compat');

// compat should be a function that takes targets and returns needed modules
assert.equal(typeof compat, 'function', 'core-js-compat should export a function');

// Call with a target to get compat data
const result = compat({ targets: 'ie 11' });
assert.equal(typeof result, 'object', 'result should be an object');

// Result should have a list of required modules
assert.ok(Array.isArray(result.list), 'result should have a list array');
assert.ok(result.list.length > 0, 'ie 11 should need many polyfills');

// The list should contain known core-js modules
const hasPromise = result.list.some((m) => m.includes('es.promise'));
assert.ok(hasPromise, 'ie 11 should need es.promise polyfill');

// Result should also have a targets mapping
assert.equal(typeof result.targets, 'object', 'result should have targets');

// Try with a modern target - should need fewer polyfills
const modernResult = compat({ targets: 'last 1 chrome version' });
assert.ok(Array.isArray(modernResult.list));
assert.ok(modernResult.list.length < result.list.length,
  'modern browser should need fewer polyfills than ie 11');

console.log('core-js-compat-test:ok');
