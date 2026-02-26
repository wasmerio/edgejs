'use strict';

// Node-compatible assert builtin. require('assert') and require('node:assert') both resolve here.
// API subset: AssertionError, ok, strictEqual, deepStrictEqual, notStrictEqual, ifError, fail, throws, rejects.
function AssertionError(message) {
  this.name = 'AssertionError';
  this.message = message || 'Assertion failed';
}
AssertionError.prototype = Object.create(Error.prototype);
AssertionError.prototype.constructor = AssertionError;

function strictEqual(actual, expected, message) {
  if (!Object.is(actual, expected)) {
    throw new AssertionError(message || ('Expected strict equality. actual=' + actual + ' expected=' + expected));
  }
}

function isDeepStrictEqual(a, b) {
  if (Object.is(a, b)) return true;
  if (typeof a !== typeof b) return false;
  if (a === null || b === null) return a === b;
  if (typeof a !== 'object') return false;

  const aIsArray = Array.isArray(a);
  const bIsArray = Array.isArray(b);
  if (aIsArray !== bIsArray) return false;

  if (aIsArray) {
    if (a.length !== b.length) return false;
    for (let i = 0; i < a.length; i += 1) {
      if (!isDeepStrictEqual(a[i], b[i])) return false;
    }
    return true;
  }

  const aKeys = Object.keys(a);
  const bKeys = Object.keys(b);
  if (aKeys.length !== bKeys.length) return false;
  for (let i = 0; i < aKeys.length; i += 1) {
    const key = aKeys[i];
    if (!Object.prototype.hasOwnProperty.call(b, key)) return false;
    if (!isDeepStrictEqual(a[key], b[key])) return false;
  }
  return true;
}

function deepStrictEqual(actual, expected, message) {
  if (!isDeepStrictEqual(actual, expected)) {
    throw new AssertionError(message || ('Expected deep equality. actual=' + JSON.stringify(actual) + ' expected=' + JSON.stringify(expected)));
  }
}

function ok(value, message) {
  if (!value) {
    throw new AssertionError(message || 'Expected truthy value');
  }
}

function notStrictEqual(actual, expected, message) {
  if (Object.is(actual, expected)) {
    throw new AssertionError(message || ('Expected strict inequality. actual=' + actual + ' expected=' + expected));
  }
}

function notDeepStrictEqual(actual, expected, message) {
  if (isDeepStrictEqual(actual, expected)) {
    throw new AssertionError(message || 'Expected values to be different');
  }
}

function match(actual, regexp, message) {
  if (!(regexp instanceof RegExp)) {
    throw new AssertionError('assert.match expects a RegExp');
  }
  if (!regexp.test(String(actual))) {
    throw new AssertionError(message || ('Expected "' + String(actual) + '" to match ' + String(regexp)));
  }
}

function doesNotMatch(actual, regexp, message) {
  if (regexp == null ||
      (Object.prototype.toString.call(regexp) !== '[object RegExp]' &&
       typeof regexp.test !== 'function')) {
    throw new AssertionError('The "regexp" argument must be a RegExp');
  }
  if (regexp.test(String(actual))) {
    throw new AssertionError(message || `Expected "${String(actual)}" not to match ${String(regexp)}`);
  }
}

function ifError(err) {
  if (err) {
    throw err;
  }
}

function fail(message) {
  throw new AssertionError(message || 'Failed');
}

function matchesExpected(err, expected) {
  if (!expected) return true;
  if (Object.prototype.toString.call(expected) === '[object RegExp]') {
    const errText = String(err);
    if (expected.test(errText) || expected.test(String(err && err.code || ''))) return true;
    try {
      const flags = expected.flags.includes('i') ? expected.flags : `${expected.flags}i`;
      const ci = new RegExp(expected.source, flags);
      return ci.test(errText);
    } catch {
      return false;
    }
  }
  if (typeof expected === 'function') {
    if (expected.prototype && (expected === Error || expected.prototype instanceof Error)) {
      return err instanceof expected;
    }
    return expected(err) === true;
  }
  if (typeof expected !== 'object') {
    return true;
  }
  if (Object.prototype.hasOwnProperty.call(expected, 'name') && err.name !== expected.name) {
    return false;
  }
  if (Object.prototype.hasOwnProperty.call(expected, 'code')) {
    const codeExpectation = expected.code;
    if ((codeExpectation && typeof codeExpectation.test === 'function')) {
      if (!codeExpectation.test(String((err && err.code) || ''))) return false;
    } else if (err.code !== codeExpectation) {
      return false;
    }
  }
  if (Object.prototype.hasOwnProperty.call(expected, 'message')) {
    const msgExpectation = expected.message;
    if (Object.prototype.toString.call(msgExpectation) === '[object RegExp]') {
      if (!msgExpectation.test(String(err.message || ''))) return false;
    } else if (String(err.message || '') !== String(msgExpectation)) {
      return false;
    }
  }
  return true;
}

function throws(fn, expected) {
  let thrown = false;
  try {
    fn();
  } catch (err) {
    thrown = true;
    if (!matchesExpected(err, expected)) {
      throw new AssertionError('Function threw unexpected error shape: ' + String(err && err.message));
    }
  }
  if (!thrown) {
    throw new AssertionError('Expected function to throw');
  }
}

function isRegExpLike(value) {
  return value != null &&
    (Object.prototype.toString.call(value) === '[object RegExp]' ||
     typeof value.test === 'function');
}

function match(actual, regexp, message) {
  if (!isRegExpLike(regexp)) {
    throw new AssertionError('The "regexp" argument must be a RegExp');
  }
  if (!regexp.test(String(actual))) {
    throw new AssertionError(message || `Expected "${String(actual)}" to match ${String(regexp)}`);
  }
}

function rejects(asyncFn, expected) {
  const p = typeof asyncFn === 'function' ? asyncFn() : asyncFn;
  if (p == null || typeof p.then !== 'function') {
    return Promise.reject(new AssertionError('Expected asyncFn to return a Promise'));
  }
  return p.then(
    function () {
      throw new AssertionError('Expected asyncFn to reject');
    },
    function (err) {
      if (!matchesExpected(err, expected)) {
        throw new AssertionError('Function rejected with unexpected error shape: ' + String(err && err.message));
      }
    }
  );
}

// Node's assert is callable: assert(value, msg) === assert.ok(value, msg)
function assert(value, message) {
  return ok(value, message);
}
assert.AssertionError = AssertionError;
assert.strictEqual = strictEqual;
assert.deepEqual = deepStrictEqual;
assert.deepStrictEqual = deepStrictEqual;
assert.ok = ok;
assert.notStrictEqual = notStrictEqual;
assert.notDeepStrictEqual = notDeepStrictEqual;
assert.ifError = ifError;
assert.fail = fail;
assert.throws = throws;
assert.rejects = rejects;
assert.match = match;
assert.doesNotMatch = doesNotMatch;
assert.strict = assert;

module.exports = assert;
