'use strict';

function runBody(fn) {
  if (typeof fn !== 'function') return;
  const t = { assert: require('assert'), mock };
  try {
    if (fn.length >= 2) {
      const done = (err) => {
        if (err) throw err;
      };
      const result = fn(t, done);
      if (result && typeof result.then === 'function') {
        result.catch((err) => {
          throw err;
        });
      }
      return;
    }
    const result = fn(t);
    if (result && typeof result.then === 'function') {
      result.catch((err) => {
        throw err;
      });
    }
  } catch (err) {
    throw err;
  }
}

function shouldSkip(options) {
  return Boolean(options && typeof options === 'object' && options.skip);
}

function test(name, options, fn) {
  if (typeof options === 'function') {
    fn = options;
    options = undefined;
  }
  if (shouldSkip(options)) return;
  runBody(fn);
}

test.skip = function skip(_name, _options, _fn) {};
test.todo = function todo(_name, _options, _fn) {};

function describe(name, options, fn) {
  if (typeof options === 'function') {
    fn = options;
    options = undefined;
  }
  if (shouldSkip(options)) return;
  runBody(fn);
}

describe.skip = function skip(_name, _options, _fn) {};

function it(name, options, fn) {
  if (typeof options === 'function') {
    fn = options;
    options = undefined;
  }
  if (shouldSkip(options)) return;
  runBody(fn);
}

it.skip = function skip(_name, _options, _fn) {};
it.todo = function todo(_name, _options, _fn) {};

function before(fn) { runBody(fn); }
function after(fn) { runBody(fn); }
function beforeEach(fn) { runBody(fn); }
function afterEach(fn) { runBody(fn); }

const mock = {
  fn(implementation) {
    const calls = [];
    function wrapped() {
      const args = Array.prototype.slice.call(arguments);
      const call = {
        arguments: args,
        result: undefined,
        error: undefined,
        this: this,
      };
      calls.push(call);
      try {
        if (typeof implementation === 'function') {
          const result = implementation.apply(this, arguments);
          call.result = result;
          return result;
        }
      } catch (err) {
        call.error = err;
        throw err;
      }
      return undefined;
    }
    wrapped.mock = { calls };
    return wrapped;
  },
  method(object, key, implementation) {
    const original = object[key];
    const calls = [];
    function wrapped() {
      const args = Array.prototype.slice.call(arguments);
      calls.push({ arguments: args });
      if (typeof implementation === 'function') {
        return implementation.apply(this, arguments);
      }
      return original.apply(this, arguments);
    }
    wrapped.wrappedMethod = original;
    object[key] = wrapped;
    return {
      mock: { calls },
      restore() {
        object[key] = original;
      },
    };
  },
};

test.test = test;
test.describe = describe;
test.it = it;
test.before = before;
test.after = after;
test.beforeEach = beforeEach;
test.afterEach = afterEach;
test.mock = mock;

module.exports = test;
module.exports.test = test;
module.exports.describe = describe;
module.exports.it = it;
module.exports.before = before;
module.exports.after = after;
module.exports.beforeEach = beforeEach;
module.exports.afterEach = afterEach;
module.exports.mock = mock;
module.exports.skip = test.skip;
module.exports.todo = test.todo;

module.exports = Object.assign(module.exports, {
  mock,
});
