'use strict';

const assert = require('node:assert/strict');

const requireFromString = require('require-from-string');

// Require a simple module that exports a value
const simple = requireFromString('module.exports = 42;');
assert.equal(simple, 42, 'should require a module that exports a number');

// Require a module that exports an object
const obj = requireFromString('module.exports = { greeting: "hello", count: 3 };');
assert.equal(obj.greeting, 'hello');
assert.equal(obj.count, 3);

// Require a module that exports a function
const fn = requireFromString('module.exports = function add(a, b) { return a + b; };');
assert.equal(typeof fn, 'function');
assert.equal(fn(2, 3), 5);

// Module using exports instead of module.exports
const namedExports = requireFromString('exports.x = 10; exports.y = 20;');
assert.equal(namedExports.x, 10);
assert.equal(namedExports.y, 20);

// Module that requires a built-in module
const withRequire = requireFromString('const path = require("node:path"); module.exports = path.sep;');
assert.equal(typeof withRequire, 'string', 'should be able to require built-in modules');

// Handle syntax errors
assert.throws(() => {
  requireFromString('this is not valid javascript }{}{');
}, 'should throw on syntax errors');

console.log('require-from-string-test:ok');
