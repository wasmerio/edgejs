'use strict';

// Stub for Node's internal/test/binding used by raw Node tests (e.g. test-fs-copyfile.js).
// Libuv errno on Unix: UV_ERR(x) = -(x). ENOENT=2, EEXIST=17.
const UV_ENOENT = -2;
const UV_EEXIST = -17;

function internalBinding(name) {
  if (name === 'uv') {
    return {
      UV_ENOENT,
      UV_EEXIST,
    };
  }
  return {};
}

module.exports = { internalBinding };
