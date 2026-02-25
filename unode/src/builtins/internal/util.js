'use strict';

const isWindows = typeof process !== 'undefined' && process.platform === 'win32';
const kEmptyObject = Object.freeze({ __proto__: null });
const customInspectSymbol = Symbol.for('nodejs.util.inspect.custom');
const kIsEncodingSymbol = Symbol('kIsEncodingSymbol');
const encodingsMap = Object.freeze({
  utf8: 1,
  utf16le: 2,
  latin1: 3,
  ascii: 4,
  base64: 5,
  base64url: 6,
  hex: 7,
});

function getLazy(initializer) {
  let initialized = false;
  let value;
  return function lazyValue() {
    if (!initialized) {
      value = initializer();
      initialized = true;
    }
    return value;
  };
}

function assignFunctionName(name, fn) {
  try {
    Object.defineProperty(fn, 'name', {
      __proto__: null,
      configurable: true,
      value: typeof name === 'symbol' ? name.description || '' : String(name),
    });
  } catch {
    // Ignore defineProperty failures for non-configurable names.
  }
  return fn;
}

const promisify = function promisify(fn) {
  return (...args) => new Promise((resolve, reject) => {
    fn(...args, (err, value) => (err ? reject(err) : resolve(value)));
  });
};
promisify.custom = Symbol.for('nodejs.util.promisify.custom');

function spliceOne(list, index) {
  for (let i = index; i + 1 < list.length; i++) {
    list[i] = list[i + 1];
  }
  list.pop();
}

function getCIDR(address, netmask, family) {
  if (typeof address !== 'string' || typeof netmask !== 'string') return null;
  if (family === 'IPv4') {
    const parts = netmask.split('.');
    if (parts.length !== 4) return null;
    let bits = 0;
    for (const part of parts) {
      const value = Number(part);
      if (!Number.isInteger(value) || value < 0 || value > 255) return null;
      bits += value.toString(2).split('').filter((ch) => ch === '1').length;
    }
    return `${address}/${bits}`;
  }
  if (family === 'IPv6') {
    const expanded = netmask.includes('::')
      ? (() => {
          const [left, right] = netmask.split('::');
          const leftParts = left ? left.split(':') : [];
          const rightParts = right ? right.split(':') : [];
          const missing = 8 - (leftParts.length + rightParts.length);
          return [...leftParts, ...Array(Math.max(0, missing)).fill('0'), ...rightParts];
        })()
      : netmask.split(':');
    if (expanded.length !== 8) return null;
    let bits = 0;
    for (const part of expanded) {
      const value = parseInt(part || '0', 16);
      if (!Number.isInteger(value) || value < 0 || value > 0xffff) return null;
      bits += value.toString(2).split('').filter((ch) => ch === '1').length;
    }
    return `${address}/${bits}`;
  }
  return null;
}

function normalizeEncoding(enc) {
  if (enc == null) return 'utf8';
  const key = String(enc).toLowerCase();
  if (key === 'utf8' || key === 'utf-8') return 'utf8';
  if (key === 'ucs2' || key === 'ucs-2' || key === 'utf16le' || key === 'utf-16le') return 'utf16le';
  if (key === 'latin1' || key === 'binary') return 'latin1';
  if (key === 'ascii') return 'ascii';
  if (key === 'base64') return 'base64';
  if (key === 'base64url') return 'base64url';
  if (key === 'hex') return 'hex';
  return undefined;
}

function defineLazyProperties(target, source, keys) {
  for (const key of keys) {
    Object.defineProperty(target, key, {
      configurable: true,
      enumerable: true,
      get() {
        const value = source[key];
        Object.defineProperty(target, key, {
          configurable: true,
          enumerable: true,
          writable: true,
          value,
        });
        return value;
      },
    });
  }
}

function deprecate(fn) {
  return fn;
}

function lazyDOMException(message, name) {
  const err = new Error(message);
  err.name = name || 'DOMException';
  return err;
}

module.exports = {
  getLazy,
  isWindows,
  assignFunctionName,
  customInspectSymbol,
  defineLazyProperties,
  deprecate,
  encodingsMap,
  kEmptyObject,
  kIsEncodingSymbol,
  lazyDOMException,
  getCIDR,
  normalizeEncoding,
  spliceOne,
  promisify,
};
