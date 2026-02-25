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

module.exports = {
  AssertionError,
  strictEqual,
  deepStrictEqual,
  throws,
};
