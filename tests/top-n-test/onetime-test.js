'use strict';

const assert = require('node:assert/strict');

(async () => {
  const mod = await import('onetime');
  const onetime = mod.default;

  // Wrap a function so it only runs once
  let callCount = 0;
  const fn = onetime(() => {
    callCount += 1;
    return 'hello';
  });

  // First call should execute the function
  assert.equal(fn(), 'hello');
  assert.equal(callCount, 1);

  // Subsequent calls should return the first result without running again
  assert.equal(fn(), 'hello');
  assert.equal(fn(), 'hello');
  assert.equal(callCount, 1, 'original function should only run once');

  // Check callCount property
  assert.equal(onetime.callCount(fn), 3, 'callCount should track total invocations');

  // Test with throw option: should throw on subsequent calls
  let throwCallCount = 0;
  const throwFn = onetime(() => {
    throwCallCount += 1;
    return 42;
  }, { throw: true });

  assert.equal(throwFn(), 42);
  assert.equal(throwCallCount, 1);

  assert.throws(() => throwFn(), 'second call with throw:true should throw');
  assert.equal(throwCallCount, 1, 'function body should not run again');

  console.log('onetime-test:ok');
})().catch((err) => {
  console.error('onetime-test:fail');
  console.error(err && err.stack ? err.stack : String(err));
  process.exit(1);
});
