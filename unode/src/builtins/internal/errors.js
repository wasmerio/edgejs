'use strict';

class AbortError extends Error {
  constructor(message = 'The operation was aborted', options = undefined) {
    super(message);
    this.name = 'AbortError';
    this.code = 'ABORT_ERR';
    if (options && 'cause' in options) {
      this.cause = options.cause;
    }
  }
}

class ERR_INVALID_ARG_VALUE extends TypeError {
  constructor(name, value) {
    super(`The argument '${name}' is invalid. Received ${String(value)}`);
    this.code = 'ERR_INVALID_ARG_VALUE';
  }
}

class ERR_INVALID_CURSOR_POS extends RangeError {
  constructor() {
    super('Cannot set cursor row/column to NaN');
    this.code = 'ERR_INVALID_CURSOR_POS';
  }
}

class ERR_USE_AFTER_CLOSE extends Error {
  constructor(name) {
    super(`${name} was closed`);
    this.code = 'ERR_USE_AFTER_CLOSE';
  }
}

class ERR_INVALID_ARG_TYPE extends TypeError {
  constructor(name, expected, actual) {
    function formatName(n) {
      const s = String(n);
      if (s.includes('.')) return `The "${s}" property`;
      if (s.endsWith(' argument')) return `The ${s}`;
      return `The "${s}" argument`;
    }
    function formatExpected(exp) {
      function joinWithOr(parts) {
        if (parts.length <= 1) return parts[0] || '';
        if (parts.length === 2) return `${parts[0]} or ${parts[1]}`;
        return `${parts.slice(0, -1).join(', ')}, or ${parts[parts.length - 1]}`;
      }
      if (Array.isArray(exp)) {
        const parts = exp.map(String);
        if (parts.length === 1) {
          if (/^[A-Z]/.test(parts[0])) return `an instance of ${parts[0]}`;
          return `of type ${parts[0]}`;
        }
        const classParts = parts.filter((p) => /^[A-Z]/.test(p));
        const typeParts = parts.filter((p) => !/^[A-Z]/.test(p));
        if (typeParts.length === 1 && classParts.length > 0) {
          const specialArrayLike = classParts.find((p) => p.toLowerCase() === 'array-like object');
          const filteredClasses = specialArrayLike ? classParts.filter((p) => p !== specialArrayLike) : classParts;
          const cls = filteredClasses.length > 0
            ? (filteredClasses.length === 1 ? filteredClasses[0] : joinWithOr(filteredClasses))
            : '';
          if (specialArrayLike) {
            if (cls) {
              return `of type ${typeParts[0]} or an instance of ${cls} or an ${specialArrayLike}`;
            }
            return `of type ${typeParts[0]} or an ${specialArrayLike}`;
          }
          return `of type ${typeParts[0]} or an instance of ${cls}`;
        }
        if (typeParts.length > 1 && classParts.length > 0) {
          const types = typeParts.length === 2
            ? `${typeParts[0]} or ${typeParts[1]}`
            : `${typeParts.slice(0, -1).join(', ')} or ${typeParts[typeParts.length - 1]}`;
          const cls = classParts.length === 1 ? classParts[0] : joinWithOr(classParts);
          return `one of type ${types} or an instance of ${cls}`;
        }
        if (parts.every((p) => /^[A-Z]/.test(p))) {
          return `an instance of ${joinWithOr(parts)}`;
        }
        return `one of type ${parts.slice(0, -1).join(', ')} or ${parts[parts.length - 1]}`;
      }
      return `of type ${String(exp)}`;
    }
    let received;
    if (actual == null) {
      received = ` Received ${actual}`;
    } else if (typeof actual === 'function') {
      received = ` Received function ${actual.name || '<anonymous>'}`;
    } else if (typeof actual === 'object') {
      if (actual.constructor?.name) {
        received = ` Received an instance of ${actual.constructor.name}`;
      } else {
        received = ` Received ${typeof actual}`;
      }
    } else {
      const str = String(actual);
      let shown = str.slice(0, 25);
      if (str.length > 25) {
        shown += '...';
      }
      if (typeof actual === 'string') {
        shown = `'${shown}'`;
      }
      received = ` Received type ${typeof actual} (${shown})`;
    }
    super(`${formatName(name)} must be ${formatExpected(expected)}.${received}`);
    this.code = 'ERR_INVALID_ARG_TYPE';
  }
}

class ERR_OUT_OF_RANGE extends RangeError {
  constructor(name, range, actual) {
    function formatReceived(v) {
      if (typeof v === 'bigint') {
        const digits = String(v < 0n ? -v : v).replace(/(\d)(?=(\d{3})+(?!\d))/g, '$1_');
        return `${v < 0n ? '-' : ''}${digits}n`;
      }
      if (name === 'value' &&
          typeof range === 'string' &&
          range.includes('2 **') &&
          typeof v === 'number' &&
          Number.isInteger(v) &&
          Number.isFinite(v) &&
          Math.abs(v) >= 1000000000) {
        const s = String(Math.abs(v)).replace(/(\d)(?=(\d{3})+(?!\d))/g, '$1_');
        return v < 0 ? `-${s}` : s;
      }
      return String(v);
    }
    super(`The value of "${name}" is out of range. It must be ${range}. Received ${formatReceived(actual)}`);
    this.code = 'ERR_OUT_OF_RANGE';
  }
}

class ERR_BUFFER_OUT_OF_BOUNDS extends RangeError {
  constructor(name = undefined) {
    super(name ? `"${name}" is outside of buffer bounds` : 'Attempt to access memory outside buffer bounds');
    this.code = 'ERR_BUFFER_OUT_OF_BOUNDS';
  }
}

class ERR_UNKNOWN_ENCODING extends TypeError {
  constructor(encoding) {
    super(`Unknown encoding: ${encoding}`);
    this.code = 'ERR_UNKNOWN_ENCODING';
  }
}

class ERR_INVALID_BUFFER_SIZE extends RangeError {
  constructor(name = '16-bits') {
    super(`Buffer size must be a multiple of ${name}`);
    this.code = 'ERR_INVALID_BUFFER_SIZE';
  }
}

class ERR_MISSING_ARGS extends TypeError {
  constructor(...args) {
    super(`The "${args.join('" and "')}" arguments must be specified`);
    this.code = 'ERR_MISSING_ARGS';
  }
}

class ERR_SYSTEM_ERROR extends Error {
  constructor(info) {
    const ctx = info || {};
    const syscall = ctx.syscall || 'unknown';
    const code = ctx.code || 'UNKNOWN';
    const message = ctx.message || 'unknown error';
    super(`A system error occurred: ${syscall} returned ${code} (${message})`);
    this.name = 'SystemError';
    this.code = 'ERR_SYSTEM_ERROR';
    this.info = ctx;
    if (ctx.errno !== undefined) this.errno = ctx.errno;
    if (ctx.syscall !== undefined) this.syscall = ctx.syscall;
  }
}
ERR_SYSTEM_ERROR.HideStackFramesError = ERR_SYSTEM_ERROR;

class ERR_UNHANDLED_ERROR extends Error {
  constructor(context) {
    const msg = context === undefined ?
      'Unhandled error.' :
      `Unhandled error. (${String(context)})`;
    super(msg);
    this.code = 'ERR_UNHANDLED_ERROR';
    this.context = context;
  }
}

const kEnhanceStackBeforeInspector = Symbol('kEnhanceStackBeforeInspector');

function hideStackFrames(fn) {
  return fn;
}

function genericNodeError(message, errorProperties) {
  const err = new Error(message);
  if (errorProperties && typeof errorProperties === 'object') {
    Object.assign(err, errorProperties);
  }
  return err;
}

module.exports = {
  AbortError,
  hideStackFrames,
  genericNodeError,
  codes: {
    ERR_INVALID_ARG_VALUE,
    ERR_INVALID_CURSOR_POS,
    ERR_USE_AFTER_CLOSE,
    ERR_INVALID_ARG_TYPE,
    ERR_OUT_OF_RANGE,
    ERR_BUFFER_OUT_OF_BOUNDS,
    ERR_UNKNOWN_ENCODING,
    ERR_INVALID_BUFFER_SIZE,
    ERR_MISSING_ARGS,
    ERR_SYSTEM_ERROR,
    ERR_UNHANDLED_ERROR,
  },
  kEnhanceStackBeforeInspector,
};
