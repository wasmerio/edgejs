'use strict';

const assert = require('node:assert/strict');

(async () => {
  const debounce = require('lodash.debounce');

  assert.equal(typeof debounce, 'function');

  // Test that debounce delays execution
  let callCount = 0;
  const debounced = debounce(() => { callCount++; }, 50);

  debounced();
  debounced();
  debounced();
  // Should not have been called yet since we're within the wait period
  assert.equal(callCount, 0, 'should not call immediately');

  // Wait for the debounce timer to expire
  await new Promise((resolve) => setTimeout(resolve, 100));
  assert.equal(callCount, 1, 'should have been called exactly once after delay');

  // Test cancel
  let cancelCount = 0;
  const cancelable = debounce(() => { cancelCount++; }, 50);
  cancelable();
  assert.equal(typeof cancelable.cancel, 'function');
  cancelable.cancel();
  await new Promise((resolve) => setTimeout(resolve, 100));
  assert.equal(cancelCount, 0, 'cancelled debounce should not fire');

  // Test flush
  let flushCount = 0;
  const flushable = debounce(() => { flushCount++; }, 200);
  flushable();
  assert.equal(typeof flushable.flush, 'function');
  flushable.flush();
  assert.equal(flushCount, 1, 'flush should invoke the debounced function immediately');

  // Test with leading option
  let leadingCount = 0;
  const leading = debounce(() => { leadingCount++; }, 50, { leading: true, trailing: false });
  leading();
  assert.equal(leadingCount, 1, 'leading debounce should fire on first call');
  leading();
  assert.equal(leadingCount, 1, 'second call within wait should not fire again');

  console.log('lodash.debounce-test:ok');
})().catch((err) => {
  console.error('lodash.debounce-test:fail');
  console.error(err && err.stack ? err.stack : String(err));
  process.exit(1);
});
