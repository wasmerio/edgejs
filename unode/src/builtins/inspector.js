'use strict';

class Session {
  connect() {}

  post(method, params, callback) {
    const cb = typeof callback === 'function' ? callback : () => {};
    if (method === 'Runtime.evaluate' && params && typeof params.expression === 'string') {
      const expr = params.expression;
      if (expr.startsWith('process.env.')) {
        const key = expr.slice('process.env.'.length);
        const value = process.env[key];
        cb(null, { result: { type: 'string', value: String(value) } });
        return;
      }
    }
    cb(null, { result: { type: 'undefined' } });
  }
}

module.exports = { Session };
