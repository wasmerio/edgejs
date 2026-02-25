'use strict';

function AssertionError(message) {
  this.name = 'AssertionError';
  this.message = message || 'Assertion failed';
}
AssertionError.prototype = Object.create(Error.prototype);
AssertionError.prototype.constructor = AssertionError;

function strictEqual(actual, expected, message) {
  if (actual !== expected) {
    throw new AssertionError(message || ('Expected strict equality. actual=' + actual + ' expected=' + expected));
  }
}

function deepStrictEqual(actual, expected, message) {
  const actualJson = JSON.stringify(actual);
  const expectedJson = JSON.stringify(expected);
  if (actualJson !== expectedJson) {
    throw new AssertionError(message || ('Expected deep equality. actual=' + actualJson + ' expected=' + expectedJson));
  }
}

function ok(value, message) {
  if (!value) {
    throw new AssertionError(message || 'Expected truthy value');
  }
}

function notStrictEqual(actual, expected, message) {
  if (actual === expected) {
    throw new AssertionError(message || ('Expected strict inequality. actual=' + actual + ' expected=' + expected));
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
  if (typeof expected === 'function') {
    return expected(err) === true;
  }
  if (typeof expected !== 'object') {
    return true;
  }
  if (Object.prototype.hasOwnProperty.call(expected, 'name') && err.name !== expected.name) {
    return false;
  }
  if (Object.prototype.hasOwnProperty.call(expected, 'code') && err.code !== expected.code) {
    return false;
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

function rejects(asyncFn, expected) {
  return Promise.resolve().then(function () {
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
  });
}

// Node's assert is callable: assert(value, msg) === assert.ok(value, msg)
function assert(value, message) {
  return ok(value, message);
}
assert.AssertionError = AssertionError;
assert.strictEqual = strictEqual;
assert.deepStrictEqual = deepStrictEqual;
assert.ok = ok;
assert.notStrictEqual = notStrictEqual;
assert.ifError = ifError;
assert.fail = fail;
assert.throws = throws;
assert.rejects = rejects;

module.exports = assert;
