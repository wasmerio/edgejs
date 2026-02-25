'use strict';

const inspectCustom = Symbol('nodejs.util.inspect.custom');

function quoteString(str) {
  return "'" + String(str).replace(/\\/g, '\\\\').replace(/'/g, "\\'") + "'";
}

function formatValue(value, opts, depth) {
  if (value === null) return 'null';
  if (value === undefined) return 'undefined';
  if (typeof value === 'string') return quoteString(value);
  if (typeof value === 'number' || typeof value === 'boolean' || typeof value === 'bigint') return String(value);
  if (typeof value === 'symbol') return value.toString();
  if (typeof value === 'function') return '[Function' + (value.name ? ': ' + value.name : '') + ']';
  if (typeof Buffer === 'function' && Buffer.isBuffer && Buffer.isBuffer(value)) {
    let inspectMax = typeof Buffer.INSPECT_MAX_BYTES === 'number' ? Buffer.INSPECT_MAX_BYTES : value.length;
    try {
      const bufferMod = require('buffer');
      if (bufferMod && typeof bufferMod.INSPECT_MAX_BYTES === 'number') {
        inspectMax = bufferMod.INSPECT_MAX_BYTES;
      }
    } catch {}
    const max = Number.isInteger(inspectMax) ? inspectMax : value.length;
    const len = Math.min(value.length, Math.max(0, max));
    const parts = [];
    for (let i = 0; i < len; i++) {
      parts.push(value[i].toString(16).padStart(2, '0'));
    }
    const remaining = value.length - len;
    const suffix = remaining > 0 ? ` ... ${remaining} more ${remaining === 1 ? 'byte' : 'bytes'}` : '';
    const extraKeys = Object.keys(value).filter((k) => {
      const n = Number(k);
      return !Number.isInteger(n) || String(n) !== k;
    });
    const props = extraKeys.length
      ? extraKeys.map((k) => `${k}: ${formatValue(value[k], opts, depth + 1)}`).join(', ')
      : '';
    const bytesPart = `${parts.join(' ')}${suffix}`.trim();
    if (bytesPart && props) return `<Buffer ${bytesPart}, ${props}>`;
    if (bytesPart) return `<Buffer ${bytesPart}>`;
    if (props) return `<Buffer ${props}>`;
    return '<Buffer >';
  }
  if (value && typeof value === 'object' && opts.customInspect !== false && typeof value[inspectCustom] === 'function') {
    return String(value[inspectCustom]());
  }
  if (Array.isArray(value)) {
    if (opts.depth !== undefined && depth >= opts.depth) return '[Array]';
    return '[ ' + value.map(function (v) { return formatValue(v, opts, depth + 1); }).join(', ') + ' ]';
  }
  if (ArrayBuffer.isView(value) && !(typeof Buffer === 'function' && Buffer.isBuffer && Buffer.isBuffer(value))) {
    if (typeof value.length === 'number' && value.length === 0) {
      const ctor = value.constructor && value.constructor.name ? value.constructor.name : 'TypedArray';
      return `${ctor}(0) []`;
    }
  }
  if (typeof value === 'object') {
    if (opts.depth !== undefined && depth >= opts.depth) return '[Object]';
    const keys = Object.keys(value);
    if (keys.length === 0) return '{}';
    const parts = keys.map(function(k) {
      return k + ': ' + formatValue(value[k], opts, depth + 1);
    });
    return '{ ' + parts.join(', ') + ' }';
  }
  return String(value);
}

function inspect(value, options) {
  const opts = options && typeof options === 'object' ? options : {};
  return formatValue(value, opts, 0);
}

function format(fmt) {
  const args = Array.prototype.slice.call(arguments, 1);
  if (typeof fmt !== 'string') {
    return [fmt].concat(args).map(function(a) { return inspect(a); }).join(' ');
  }
  let idx = 0;
  const out = fmt.replace(/%[sdijfoO%]/g, function(token) {
    if (token === '%%') return '%';
    if (idx >= args.length) return token;
    const val = args[idx++];
    switch (token) {
      case '%s': return String(val);
      case '%d':
      case '%i': return Number.parseInt(val, 10).toString();
      case '%f': return Number(val).toString();
      case '%j':
        try { return JSON.stringify(val); } catch (_) { return '[Circular]'; }
      case '%o':
      case '%O': return inspect(val);
      default: return String(val);
    }
  });
  if (idx < args.length) {
    return out + ' ' + args.slice(idx).map(function(a) {
      if (a === null || a === undefined) return String(a);
      if (typeof a === 'string' || typeof a === 'number' || typeof a === 'boolean' || typeof a === 'bigint' ||
          typeof a === 'symbol') {
        return String(a);
      }
      return inspect(a);
    }).join(' ');
  }
  return out;
}

module.exports = {
  inspect,
  format,
};
module.exports.inspect.custom = inspectCustom;
module.exports.inspect.defaultOptions = {
  maxArrayLength: Infinity,
};

class TextEncoder {
  encode(input = '') {
    return Buffer.from(String(input), 'utf8');
  }
}

class TextDecoder {
  constructor(encoding = 'utf-8') {
    this.encoding = String(encoding || 'utf-8').toLowerCase();
  }
  decode(input) {
    const bytes = Buffer.from(input || []);
    return bytes.toString(this.encoding === 'utf-8' ? 'utf8' : this.encoding);
  }
}

module.exports.TextEncoder = TextEncoder;
module.exports.TextDecoder = TextDecoder;
if (typeof globalThis.TextEncoder !== 'function') globalThis.TextEncoder = TextEncoder;
if (typeof globalThis.TextDecoder !== 'function') globalThis.TextDecoder = TextDecoder;
