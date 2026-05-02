'use strict';

const assert = require('node:assert/strict');
const { Readable } = require('node:stream');

const destroy = require('destroy');

// Create a readable stream
const stream = new Readable({
  read() {
    this.push('data');
  },
});

// Stream should not be destroyed initially
assert.equal(stream.destroyed, false);

// Destroy it using the destroy module
const result = destroy(stream);

// destroy() returns the stream
assert.equal(result, stream);

// Stream should now be destroyed
assert.equal(stream.destroyed, true);

console.log('destroy-test:ok');
