'use strict';

const assert = require('node:assert/strict');

(async () => {
  const setBlocking = require('set-blocking');

  // It should be a function
  assert.equal(typeof setBlocking, 'function');

  // Call with true on stdout -- should not throw
  setBlocking(true);

  // Call with false -- should not throw
  setBlocking(false);

  // Restore to blocking mode (the default for Node)
  setBlocking(true);

  console.log('set-blocking-test:ok');
})().catch((err) => {
  console.error('set-blocking-test:fail');
  console.error(err && err.stack ? err.stack : String(err));
  process.exit(1);
});
