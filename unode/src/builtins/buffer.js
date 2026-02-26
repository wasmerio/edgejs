'use strict';

const path = require('path');
const { internalBinding } = require('internal/test/binding_runtime');

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

function wrapGenericSearchMethod(name) {
  const original = Buffer.prototype[name];
  if (typeof original !== 'function') return;
  Buffer.prototype[name] = function wrappedSearchMethod(...args) {
    if (!Buffer.isBuffer(this) && ArrayBuffer.isView(this)) {
      return original.apply(Buffer.from(this), args);
    }
    return original.apply(this, args);
  };
}

wrapGenericSearchMethod('includes');

function attachUint8ArrayAlias(name) {
  if (typeof Buffer.prototype[name] !== 'function') return;
  if (typeof Uint8Array.prototype[name] === 'function') return;
  Object.defineProperty(Uint8Array.prototype, name, {
    configurable: true,
    writable: true,
    value: Buffer.prototype[name],
  });
}

[
  'asciiSlice',
  'base64Slice',
  'base64urlSlice',
  'latin1Slice',
  'hexSlice',
  'ucs2Slice',
  'utf8Slice',
  'asciiWrite',
  'base64Write',
  'base64urlWrite',
  'latin1Write',
  'hexWrite',
  'ucs2Write',
  'utf8Write',
].forEach(attachUint8ArrayAlias);

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

if (typeof Buffer.allocUnsafeSlow === 'function') {
  const originalAllocUnsafeSlow = Buffer.allocUnsafeSlow;
  Buffer.allocUnsafeSlow = function allocUnsafeSlow(size) {
    const n = Number(size);
    // Avoid fatal OOM on very large requests in constrained test runtime.
    if (!Number.isFinite(n) || n < 0 || n >= (2 ** 30)) {
      const err = new RangeError(`The value of "size" is out of range. Received ${size}`);
      err.code = 'ERR_OUT_OF_RANGE';
      throw err;
    }
    return originalAllocUnsafeSlow(size);
  };
}

// Do not override INSPECT_MAX_BYTES; Node's buffer.js uses its own module-scope
// variable for Buffer's [customInspectSymbol], so the property must remain
// Node's getter/setter for the inspect test (test-buffer-inspect.js) to pass.

module.exports = exported;
