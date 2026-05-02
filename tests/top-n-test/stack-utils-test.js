'use strict';

const assert = require('node:assert/strict');

const StackUtils = require('stack-utils');

// Create an instance
const stack = new StackUtils({ cwd: process.cwd() });

// Generate a real stack trace and clean it
const err = new Error('test error');
const cleaned = stack.clean(err.stack);
assert.equal(typeof cleaned, 'string', 'cleaned stack should be a string');
assert.ok(cleaned.length > 0, 'cleaned stack should not be empty');

// Parse a single stack line
const line = '    at Object.<anonymous> (/home/user/project/test.js:10:15)';
const parsed = stack.parseLine(line);
assert.ok(parsed !== null, 'parseLine should return an object');
assert.equal(typeof parsed, 'object');
assert.equal(parsed.file, '/home/user/project/test.js');
assert.equal(parsed.line, 10);
assert.equal(parsed.column, 15);

// Parse another format
const namedLine = '    at myFunction (/some/file.js:42:5)';
const namedParsed = stack.parseLine(namedLine);
assert.ok(namedParsed !== null);
assert.equal(namedParsed.function, 'myFunction');
assert.equal(namedParsed.file, '/some/file.js');
assert.equal(namedParsed.line, 42);
assert.equal(namedParsed.column, 5);

// Capture a stack trace from the current point
const captured = stack.capture();
assert.ok(Array.isArray(captured), 'capture should return an array');
assert.ok(captured.length > 0, 'should capture at least one frame');

console.log('stack-utils-test:ok');
