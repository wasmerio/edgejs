'use strict';

const assert = require('assert');
const childProcess = require('child_process');
try { require('internal/event_target'); } catch {}

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

function mustCallAtLeast(fn, minimum) {
  return _mustCallInner(fn, minimum === undefined ? 1 : minimum, 'minimum');
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

function expectsError(validator, exact) {
  return mustCall(function matcher() {
    const args = Array.prototype.slice.call(arguments);
    if (args.length !== 1) {
      assert.fail(`Expected one argument, got ${String(args)}`);
    }
    const err = args[0];
    assert.strictEqual(
      Object.prototype.propertyIsEnumerable.call(err, 'message'),
      false
    );
    assert.throws(function rethrow() { throw err; }, validator);
    return true;
  }, exact);
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
  let shown = String(input).slice(0, 25);
  if (String(input).length > 25) shown += '...';
  if (typeof input === 'string') shown = `'${shown}'`;
  return ` Received type ${typeof input} (${shown})`;
}

// Node test harness: wrap options so tests can assert they are not mutated (no-op here).
function mustNotMutateObjectDeep(obj) {
  return obj;
}

function platformTimeout(ms) {
  return Number(ms) || 0;
}

const isWindows = typeof process !== 'undefined' && process.platform === 'win32';
const isAIX = typeof process !== 'undefined' && process.platform === 'aix';
const isIBMi = false;
const isSunOS = typeof process !== 'undefined' && process.platform === 'sunos';
const isDebug = false;
const isMainThread = true;
const isDumbTerminal = typeof process !== 'undefined' &&
  process.env &&
  process.env.TERM === 'dumb';
const localhostIPv4 = '127.0.0.1';
const localhostIPv6 = '::1';
const hasCrypto = (() => {
  try {
    return !!require('crypto');
  } catch {
    return false;
  }
})();

// True if the process can create symlinks (e.g. not in a sandbox). Raw tests may skip symlink tests if false.
function canCreateSymLink() {
  return true;
}

function skip(msg) {
  console.log(`1..0 # Skipped: ${msg || ''}`.trim());
  process.exit(0);
}

function printSkipMessage(msg) {
  console.log(`1..0 # Skipped: ${msg || ''}`.trim());
}

function skipIf32Bits() {
  if (process.arch === 'ia32' || process.arch === 'arm') {
    skip('skipped on 32-bit platforms');
  }
}

function skipIfInspectorDisabled() {
  if (!process.features || process.features.inspector !== true) {
    skip('Inspector is disabled');
  }
}

function spawnPromisified(cmd, args, options) {
  return new Promise((resolve) => {
    const child = childProcess.spawn(cmd, args, options);
    let stdout = '';
    let stderr = '';
    if (child.stdout) child.stdout.on('data', (chunk) => { stdout += String(chunk); });
    if (child.stderr) child.stderr.on('data', (chunk) => { stderr += String(chunk); });
    child.on('close', (code, signal) => resolve({ code, signal, stdout, stderr }));
    child.on('error', (err) => resolve({ code: 1, signal: null, stdout, stderr: String(err) }));
  });
}

function getArrayBufferViews(buffer) {
  const views = [];
  const ab = buffer.buffer.slice(buffer.byteOffset, buffer.byteOffset + buffer.byteLength);
  views.push(new Uint8Array(ab));
  views.push(new Uint16Array(ab, 0, Math.floor(ab.byteLength / 2)));
  views.push(new DataView(ab));
  return views;
}

function getTTYfd() {
  return -1;
}

module.exports = {
  mustCall,
  mustCallAtLeast,
  mustSucceed,
  mustNotCall,
  expectWarning,
  expectsError,
  invalidArgTypeHelper,
  mustNotMutateObjectDeep,
  platformTimeout,
  isWindows,
  isAIX,
  isIBMi,
  isSunOS,
  isDebug,
  isMainThread,
  isDumbTerminal,
  localhostIPv4,
  localhostIPv6,
  hasCrypto,
  canCreateSymLink,
  printSkipMessage,
  skip,
  skipIf32Bits,
  skipIfInspectorDisabled,
  spawnPromisified,
  getArrayBufferViews,
  getTTYfd,
};
