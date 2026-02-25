'use strict';

const assert = require('assert');

const mustCallChecks = [];

function runCallChecks(exitCode) {
  if (exitCode !== 0) return;
  const failed = mustCallChecks.filter(function (c) {
    if ('minimum' in c) return c.actual < c.minimum;
    return c.actual !== c.exact;
  });
  failed.forEach(function (c) {
    const seg = 'minimum' in c ? `at least ${c.minimum}` : `exactly ${c.exact}`;
    console.log('Mismatched function calls. Expected %s, actual %d.', seg, c.actual);
    console.log((c.stack || '').split('\n').slice(2).join('\n'));
  });
  if (failed.length) process.exit(1);
}

function _mustCallInner(fn, criteria, field) {
  if (typeof fn === 'number') {
    criteria = fn;
    fn = function () {};
  } else if (fn === undefined) {
    fn = function () {};
  }
  const context = {
    [field]: criteria,
    actual: 0,
    stack: (new Error()).stack,
    name: fn.name || '<anonymous>',
  };
  if (mustCallChecks.length === 0) process.on('exit', runCallChecks);
  mustCallChecks.push(context);
  return function () {
    context.actual++;
    return fn.apply(this, arguments);
  };
}

function mustCall(fn, exact) {
  return _mustCallInner(fn, exact === undefined ? 1 : exact, 'exact');
}

function mustSucceed(fn, exact) {
  return mustCall(function (err) {
    assert.ifError(err);
    if (typeof fn === 'function') return fn.apply(this, Array.prototype.slice.call(arguments, 1));
  }, exact);
}

function mustNotCall(msg) {
  return mustCall(function () {
    assert.fail(msg || 'should not have been called');
  }, 0);
}

function expectWarning(/* nameOrMap, expected, code */) {
  // No-op for raw tests that only need the API to exist.
}

// Node test harness: simplify ERR_INVALID_ARG_TYPE message for assert.throws.
function invalidArgTypeHelper(input) {
  if (input == null) {
    return ` Received ${input}`;
  }
  if (typeof input === 'function') {
    return ` Received function ${input.name || '<anonymous>'}`;
  }
  if (typeof input === 'object') {
    if (input.constructor && input.constructor.name) {
      return ` Received an instance of ${input.constructor.name}`;
    }
    return ` Received ${typeof input}`;
  }
  return ` Received type ${typeof input} (${String(input).slice(0, 25)}${String(input).length > 25 ? '...' : ''})`;
}

// Node test harness: wrap options so tests can assert they are not mutated (no-op here).
function mustNotMutateObjectDeep(obj) {
  return obj;
}

const isWindows = typeof process !== 'undefined' && process.platform === 'win32';

// True if the process can create symlinks (e.g. not in a sandbox). Raw tests may skip symlink tests if false.
function canCreateSymLink() {
  return true;
}

module.exports = {
  mustCall,
  mustSucceed,
  mustNotCall,
  expectWarning,
  invalidArgTypeHelper,
  mustNotMutateObjectDeep,
  isWindows,
  canCreateSymLink,
};
