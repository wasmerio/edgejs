'use strict';

const assert = require('node:assert/strict');

const toIdentifier = require('toidentifier');

// Convert strings with spaces to PascalCase identifiers
assert.equal(toIdentifier('foo bar'), 'FooBar', 'should convert "foo bar" to "FooBar"');
assert.equal(toIdentifier('hello world'), 'HelloWorld', 'should convert spaces to camel-cased');

// Single word gets capitalized
assert.equal(toIdentifier('hello'), 'Hello', 'single word should be capitalized');

// Already an identifier
assert.equal(toIdentifier('FooBar'), 'FooBar', 'already PascalCase should stay the same');

// Strings with special characters
assert.equal(typeof toIdentifier('foo-bar'), 'string', 'should handle dashes');
assert.equal(typeof toIdentifier('foo_bar'), 'string', 'should handle underscores');

// Empty string
assert.equal(toIdentifier(''), '', 'empty string should return empty string');

// Numbers in the string
const withNumbers = toIdentifier('content type');
assert.equal(withNumbers, 'ContentType', 'should handle multi-word conversion');

// HTTP status-like strings (common use case for this package)
const notFound = toIdentifier('not found');
assert.equal(notFound, 'NotFound');

const internalError = toIdentifier('internal server error');
assert.equal(internalError, 'InternalServerError');

console.log('toidentifier-test:ok');
