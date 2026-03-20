'use strict';

const assert = require('node:assert/strict');

const { fn } = require('jest-mock');

// Create a mock function and call it
const mock = fn();
assert.equal(typeof mock, 'function');
mock('hello');
mock('world');

// Track calls
assert.equal(mock.mock.calls.length, 2);
assert.deepEqual(mock.mock.calls[0], ['hello']);
assert.deepEqual(mock.mock.calls[1], ['world']);

// Set return value
const mockWithReturn = fn();
mockWithReturn.mockReturnValue(42);
assert.equal(mockWithReturn(), 42);
assert.equal(mockWithReturn(), 42);

// mockImplementation
const mockImpl = fn();
mockImpl.mockImplementation((x) => x * 2);
assert.equal(mockImpl(5), 10);
assert.equal(mockImpl(3), 6);
assert.equal(mockImpl.mock.calls.length, 2);

// mockReset clears calls and return values
const mockReset = fn();
mockReset.mockReturnValue('before');
assert.equal(mockReset(), 'before');
mockReset.mockReset();
assert.equal(mockReset(), undefined);
assert.equal(mockReset.mock.calls.length, 1);

// mockReturnValueOnce
const mockOnce = fn();
mockOnce.mockReturnValueOnce('first').mockReturnValueOnce('second');
assert.equal(mockOnce(), 'first');
assert.equal(mockOnce(), 'second');
assert.equal(mockOnce(), undefined);

console.log('jest-mock-test:ok');
