'use strict';

const assert = require('node:assert/strict');

const isSharedArrayBuffer = require('is-shared-array-buffer');

// Returns false for regular ArrayBuffer
assert.equal(isSharedArrayBuffer(new ArrayBuffer(8)), false);

// Returns false for other types
assert.equal(isSharedArrayBuffer(null), false);
assert.equal(isSharedArrayBuffer(undefined), false);
assert.equal(isSharedArrayBuffer(42), false);
assert.equal(isSharedArrayBuffer('string'), false);
assert.equal(isSharedArrayBuffer({}), false);
assert.equal(isSharedArrayBuffer([]), false);
assert.equal(isSharedArrayBuffer(new Uint8Array(4)), false);

// Returns true for SharedArrayBuffer if available
if (typeof SharedArrayBuffer !== 'undefined') {
  const sab = new SharedArrayBuffer(8);
  assert.equal(isSharedArrayBuffer(sab), true);
}

console.log('is-shared-array-buffer-test:ok');
