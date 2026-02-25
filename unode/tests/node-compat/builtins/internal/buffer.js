'use strict';

const path = require('path');
const { internalBinding, primordials } = require('internal/test/binding');

if (typeof globalThis.internalBinding !== 'function') {
  globalThis.internalBinding = internalBinding;
}
if (!globalThis.primordials) {
  globalThis.primordials = primordials;
}

module.exports = require(path.resolve(__dirname, '../../../../../node/lib/internal/buffer.js'));
