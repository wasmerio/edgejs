'use strict';

const {
  codes: {
    ERR_INVALID_ARG_TYPE,
    ERR_INVALID_ARG_VALUE,
    ERR_OUT_OF_RANGE,
    ERR_SOCKET_BAD_PORT,
  },
} = require('internal/errors');

function validateFunction(value, name) {
  if (typeof value !== 'function') {
    throw new ERR_INVALID_ARG_TYPE(name, 'function', value);
  }
}

function validateAbortSignal(value, name) {
  if (value === undefined) return;
  if (typeof value !== 'object' || typeof value.aborted !== 'boolean') {
    throw new ERR_INVALID_ARG_TYPE(name, 'AbortSignal', value);
  }
}

function validateString(value, name) {
  if (typeof value !== 'string') {
    throw new ERR_INVALID_ARG_TYPE(name, 'string', value);
  }
}

function validateUint32(value, name, positive) {
  if (typeof value !== 'number') {
    throw new ERR_INVALID_ARG_TYPE(name, 'number', value);
  }
  if (!Number.isInteger(value)) {
    throw new ERR_OUT_OF_RANGE(name, '>= 0 && <= 4294967295', value);
  }
  if (value < 0 || value > 0xFFFFFFFF || (positive && value === 0)) {
    throw new ERR_OUT_OF_RANGE(name, '>= 0 && <= 4294967295', value);
  }
}

function validateObject(value, name, options = undefined) {
  const allowArray = options?.allowArray === true;
  const allowFunction = options?.allowFunction === true;
  const nullable = options?.nullable === true;

  if ((!nullable && value === null) ||
      (!allowArray && Array.isArray(value)) ||
      (typeof value !== 'object' && (!allowFunction || typeof value !== 'function'))) {
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

function isInt32(value) {
  return typeof value === 'number' &&
    Number.isInteger(value) &&
    value >= -2147483648 &&
    value <= 2147483647;
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
  if ((min !== undefined && value < min) ||
      (max !== undefined && value > max) ||
      ((min !== undefined || max !== undefined) && Number.isNaN(value))) {
    const range = `${min !== undefined ? `>= ${min}` : ''}` +
      `${min !== undefined && max !== undefined ? ' && ' : ''}` +
      `${max !== undefined ? `<= ${max}` : ''}`;
    throw new ERR_OUT_OF_RANGE(name, range, value);
  }
}

function validateOneOf(value, name, oneOf) {
  if (!Array.isArray(oneOf) || !oneOf.includes(value)) {
    const formatted = Array.isArray(oneOf)
      ? oneOf.map((v) => (typeof v === 'string' ? `'${v}'` : String(v))).join(', ')
      : String(oneOf);
    const label = String(name).includes('.') ? 'property' : 'argument';
    const received = typeof value === 'string' ? `'${value}'` : String(value);
    const err = new TypeError(
      `The ${label} '${name}' must be one of: ${formatted}. Received ${received}`
    );
    err.code = 'ERR_INVALID_ARG_VALUE';
    throw err;
  }
}

function validatePort(value, name = 'Port', allowZero = true) {
  if ((typeof value !== 'number' && typeof value !== 'string') ||
      (typeof value === 'string' && value.trim().length === 0)) {
    throw new ERR_SOCKET_BAD_PORT(name, value, allowZero);
  }
  const port = +value;
  if ((port >>> 0) !== port || port > 0xFFFF || (port === 0 && !allowZero)) {
    throw new ERR_SOCKET_BAD_PORT(name, value, allowZero);
  }
  return port | 0;
}

module.exports = {
  validateArray,
  validateObject,
  validateAbortSignal,
  validateBoolean,
  validateBuffer,
  validateFunction,
  isInt32,
  validateInt32,
  validateInteger,
  validateNumber,
  validateOneOf,
  validatePort,
  validateString,
  validateUint32,
};
