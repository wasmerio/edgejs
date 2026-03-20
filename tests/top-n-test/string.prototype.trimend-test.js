'use strict';

const assert = require('node:assert/strict');

const trimEnd = require('string.prototype.trimend');

// Trim trailing spaces
assert.equal(trimEnd('hello   '), 'hello');

// Trim trailing tabs
assert.equal(trimEnd('hello\t\t'), 'hello');

// Trim trailing newlines
assert.equal(trimEnd('hello\n\n'), 'hello');

// Trim mixed trailing whitespace
assert.equal(trimEnd('hello \t\n '), 'hello');

// No-op on already trimmed string
assert.equal(trimEnd('hello'), 'hello');

// Preserves leading whitespace
assert.equal(trimEnd('  hello  '), '  hello');

// Works on empty string
assert.equal(trimEnd(''), '');

// Works on all-whitespace string
assert.equal(trimEnd('   '), '');

console.log('string.prototype.trimend-test:ok');
