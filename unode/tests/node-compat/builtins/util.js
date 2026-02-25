'use strict';

function inspect(obj, options) {
  if (obj === null) return 'null';
  if (obj === undefined) return 'undefined';
  if (typeof obj === 'string') return obj;
  if (typeof obj === 'number' || typeof obj === 'boolean') return String(obj);
  if (typeof obj === 'symbol') return obj.toString();
  if (typeof obj !== 'object') return String(obj);
  try {
    if (Array.isArray(obj)) return '[' + Array.prototype.map.call(obj, function (v) { return inspect(v, options); }).join(', ') + ']';
    if (Object.prototype.toString.call(obj) === '[object Arguments]') return inspect(Array.prototype.slice.call(obj), options);
    const keys = Object.keys(obj);
    const parts = keys.map(function (k) { return k + ': ' + inspect(obj[k], options); });
    return '{ ' + parts.join(', ') + ' }';
  } catch (e) {
    return '[Object]';
  }
}

module.exports = { inspect };
