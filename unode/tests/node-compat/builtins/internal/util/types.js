'use strict';

function isAnyArrayBuffer(value) {
  if (value == null || typeof value !== 'object') return false;
  try {
    if (value instanceof ArrayBuffer) {
      return typeof value.byteLength === 'number';
    }
    if (typeof SharedArrayBuffer === 'function' && value instanceof SharedArrayBuffer) {
      return typeof value.byteLength === 'number';
    }
  } catch {
    return false;
  }
  return false;
}

function isArrayBufferView(value) {
  return ArrayBuffer.isView(value);
}

function isUint8Array(value) {
  try {
    return ArrayBuffer.isView(value) &&
      !(value instanceof DataView) &&
      value != null &&
      typeof value.BYTES_PER_ELEMENT === 'number' &&
      value.BYTES_PER_ELEMENT === 1;
  } catch {
    return false;
  }
}

function isTypedArray(value) {
  return ArrayBuffer.isView(value) && !(value instanceof DataView);
}

module.exports = {
  isAnyArrayBuffer,
  isArrayBufferView,
  isTypedArray,
  isUint8Array,
};
