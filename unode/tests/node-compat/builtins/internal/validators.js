'use strict';

const {
  codes: {
    ERR_INVALID_ARG_TYPE,
    ERR_INVALID_ARG_VALUE,
    ERR_OUT_OF_RANGE,
  },
} = require('internal/errors');

function validateFunction(value, name) {
  if (typeof value !== 'function') {
    throw new ERR_INVALID_ARG_TYPE(name, 'Function', value);
  }
}

function validateAbortSignal(value, name) {
  if (!value || typeof value !== 'object' || typeof value.aborted !== 'boolean') {
    throw new ERR_INVALID_ARG_TYPE(name, 'AbortSignal', value);
  }
}

function validateString(value, name) {
  if (typeof value !== 'string') {
    throw new ERR_INVALID_ARG_TYPE(name, 'string', value);
  }
}

function validateUint32(value, name, positive) {
  if (!Number.isInteger(value)) {
    throw new ERR_INVALID_ARG_TYPE(name, 'number', value);
  }
  if (value < 0 || value > 0xFFFFFFFF || (positive && value === 0)) {
    throw new ERR_INVALID_ARG_VALUE(name, value);
  }
}

function validateObject(value, name) {
  if (value === null || typeof value !== 'object') {
    throw new ERR_INVALID_ARG_TYPE(name, 'object', value);
  }
}

function validateBoolean(value, name) {
  if (typeof value !== 'boolean') {
    throw new ERR_INVALID_ARG_TYPE(name, 'boolean', value);
  }
}

function validateInteger(value, name, min = Number.MIN_SAFE_INTEGER, max = Number.MAX_SAFE_INTEGER) {
  if (typeof value !== 'number') {
    throw new ERR_INVALID_ARG_TYPE(name, 'number', value);
  }
  if (!Number.isInteger(value)) {
    throw new ERR_OUT_OF_RANGE(name, 'an integer', value);
  }
  if (value < min || value > max) {
    throw new ERR_OUT_OF_RANGE(name, `>= ${min} && <= ${max}`, value);
  }
}

function validateInt32(value, name, min = -2147483648, max = 2147483647) {
  if (typeof value !== 'number') {
    throw new ERR_INVALID_ARG_TYPE(name, 'number', value);
  }
  if (!Number.isInteger(value) || Number.isNaN(value) || value < min || value > max) {
    throw new ERR_OUT_OF_RANGE(name, `>= ${min} && <= ${max}`, value);
  }
}

function validateArray(value, name) {
  if (!Array.isArray(value)) {
    throw new ERR_INVALID_ARG_TYPE(name, ['Array'], value);
  }
}

function validateBuffer(value, name = 'buffer') {
  const isBufferLike = value != null &&
    typeof value === 'object' &&
    typeof value.byteLength === 'number' &&
    (typeof value.length === 'number' || ArrayBuffer.isView(value));
  if (!isBufferLike) {
    throw new ERR_INVALID_ARG_TYPE(name, ['Buffer', 'TypedArray', 'DataView'], value);
  }
}

function validateNumber(value, name, min, max) {
  if (typeof value !== 'number') {
    throw new ERR_INVALID_ARG_TYPE(name, 'number', value);
  }
  if ((min !== undefined || max !== undefined) && !Number.isFinite(value)) {
    throw new ERR_OUT_OF_RANGE(name, 'a finite number', value);
  }
  if (min !== undefined && value < min) {
    throw new ERR_OUT_OF_RANGE(name, `>= ${min}`, value);
  }
  if (max !== undefined && value > max) {
    throw new ERR_OUT_OF_RANGE(name, `<= ${max}`, value);
  }
}

module.exports = {
  validateArray,
  validateObject,
  validateAbortSignal,
  validateBoolean,
  validateBuffer,
  validateFunction,
  validateInt32,
  validateInteger,
  validateNumber,
  validateString,
  validateUint32,
};
