'use strict';

const path = require('path');
const { internalBinding, primordials } = require('internal/test/binding');

if (typeof globalThis.internalBinding !== 'function') {
  globalThis.internalBinding = internalBinding;
}
if (!globalThis.primordials) {
  globalThis.primordials = primordials;
}

const exported = require(path.resolve(__dirname, '../../../../node/lib/buffer.js'));
const { Buffer } = exported;

function isDetachedArrayBuffer(input) {
  const ab = input instanceof ArrayBuffer ? input :
    (ArrayBuffer.isView(input) ? input.buffer : null);
  return !!(ab && globalThis.__unode_detached_arraybuffers &&
    globalThis.__unode_detached_arraybuffers.has(ab));
}

function makeInvalidStateError() {
  const err = new Error('The ArrayBuffer is detached');
  err.name = 'TypeError';
  err.code = 'ERR_INVALID_STATE';
  return err;
}

if (typeof Buffer.isUtf8 === 'function') {
  const originalIsUtf8 = Buffer.isUtf8;
  Buffer.isUtf8 = function isUtf8(input) {
    if (isDetachedArrayBuffer(input)) throw makeInvalidStateError();
    return originalIsUtf8(input);
  };
}

if (typeof Buffer.isAscii === 'function') {
  const originalIsAscii = Buffer.isAscii;
  Buffer.isAscii = function isAscii(input) {
    if (isDetachedArrayBuffer(input)) throw makeInvalidStateError();
    return originalIsAscii(input);
  };
}

function validateIsEncodingInput(input) {
  const isAB = input instanceof ArrayBuffer || (typeof SharedArrayBuffer === 'function' && input instanceof SharedArrayBuffer);
  const isView = ArrayBuffer.isView(input);
  if (!isAB && !isView) {
    const err = new TypeError(
      'The "input" argument must be an instance of ArrayBuffer, Buffer, or ArrayBufferView.'
    );
    err.code = 'ERR_INVALID_ARG_TYPE';
    throw err;
  }
}

if (typeof exported.isUtf8 === 'function') {
  const original = exported.isUtf8;
  exported.isUtf8 = function isUtf8(input) {
    validateIsEncodingInput(input);
    if (isDetachedArrayBuffer(input)) throw makeInvalidStateError();
    return original(input);
  };
}

if (typeof exported.isAscii === 'function') {
  const original = exported.isAscii;
  exported.isAscii = function isAscii(input) {
    validateIsEncodingInput(input);
    if (isDetachedArrayBuffer(input)) throw makeInvalidStateError();
    return original(input);
  };
}

let inspectMaxBytes = typeof exported.INSPECT_MAX_BYTES === 'number' ? exported.INSPECT_MAX_BYTES : 50;
Object.defineProperty(exported, 'INSPECT_MAX_BYTES', {
  configurable: true,
  enumerable: true,
  get() {
    return inspectMaxBytes;
  },
  set(value) {
    if (typeof value !== 'number') {
      const err = new TypeError('The "value" argument must be of type number');
      err.code = 'ERR_INVALID_ARG_TYPE';
      throw err;
    }
    if ((Number.isFinite(value) && value < 0) || Number.isNaN(value)) {
      const err = new RangeError('The value of "value" is out of range');
      err.code = 'ERR_OUT_OF_RANGE';
      throw err;
    }
    inspectMaxBytes = Number.isFinite(value) ? Math.trunc(value) : value;
  },
});

module.exports = exported;
