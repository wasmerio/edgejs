'use strict';

function runWithContext(code, context, options) {
  const sandbox = context && typeof context === 'object' ? context : {};
  const keys = Object.keys(sandbox);
  const hadOwn = Object.create(null);
  const previous = Object.create(null);

  for (const key of keys) {
    hadOwn[key] = Object.prototype.hasOwnProperty.call(globalThis, key);
    if (hadOwn[key]) {
      previous[key] = globalThis[key];
    }
    globalThis[key] = sandbox[key];
  }

  try {
    let source = String(code);
    if (options && typeof options === 'object' && typeof options.filename === 'string' && options.filename.length > 0) {
      source += `\n//# sourceURL=${options.filename}`;
    }
    return (0, eval)(source);
  } finally {
    for (const key of keys) {
      if (hadOwn[key]) {
        globalThis[key] = previous[key];
      } else {
        delete globalThis[key];
      }
    }
  }
}

function runInNewContext(code, context, options) {
  const result = runWithContext(code, context, options);
  if (typeof result === 'function') {
    const altProto = Object.create(Function.prototype);
    Object.setPrototypeOf(result, altProto);
  }
  return result;
}

function runInThisContext(code, options) {
  let source = String(code);
  if (options && typeof options === 'object' && typeof options.filename === 'string' && options.filename.length > 0) {
    source += `\n//# sourceURL=${options.filename}`;
  }
  return (0, eval)(source);
}

module.exports = {
  runInNewContext,
  runInThisContext,
};
