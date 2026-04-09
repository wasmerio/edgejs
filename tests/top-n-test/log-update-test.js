'use strict';

const assert = require('node:assert/strict');

(async () => {
  const logUpdate = await import('log-update');
  const fn = logUpdate.default || logUpdate;

  // log-update exports a function as its default export
  assert.equal(typeof fn, 'function', 'default export should be a function');

  // It should have a .clear method
  assert.equal(typeof fn.clear, 'function', 'should have a .clear method');

  // It should have a .done method
  assert.equal(typeof fn.done, 'function', 'should have a .done method');

  // It should also export createLogUpdate for creating custom instances
  const createLogUpdate = logUpdate.createLogUpdate;
  assert.equal(typeof createLogUpdate, 'function', 'should export createLogUpdate');

  console.log('log-update-test:ok');
})().catch((err) => {
  console.error('log-update-test:fail');
  console.error(err && err.stack ? err.stack : String(err));
  process.exit(1);
});
