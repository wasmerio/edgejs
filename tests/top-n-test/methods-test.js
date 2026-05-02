'use strict';

const assert = require('node:assert/strict');

const methods = require('methods');

// It should be an array
assert.ok(Array.isArray(methods), 'methods should be an array');
assert.ok(methods.length > 0, 'should have some methods');

// Should contain the common HTTP methods (lowercase)
const expected = ['get', 'post', 'put', 'delete', 'patch'];
for (const method of expected) {
  assert.ok(methods.includes(method), `should include ${method}`);
}

// All entries should be lowercase strings
for (const method of methods) {
  assert.equal(typeof method, 'string', 'each method should be a string');
  assert.equal(method, method.toLowerCase(), `${method} should be lowercase`);
}

// Should also include other well-known HTTP methods
assert.ok(methods.includes('head'), 'should include head');
assert.ok(methods.includes('options'), 'should include options');

console.log('methods-test:ok');
