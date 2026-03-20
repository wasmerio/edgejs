'use strict';

const assert = require('node:assert/strict');
const once = require('lodash.once');

try {
  // Track how many times the underlying function is actually called
  let callCount = 0;
  const greet = once(function (name) {
    callCount += 1;
    return 'Hello, ' + name + '!';
  });

  // First call should work normally
  const first = greet('Alice');
  assert.equal(first, 'Hello, Alice!');
  assert.equal(callCount, 1);

  // Subsequent calls should return the first result, not re-execute
  const second = greet('Bob');
  assert.equal(second, 'Hello, Alice!');
  assert.equal(callCount, 1);

  const third = greet('Charlie');
  assert.equal(third, 'Hello, Alice!');
  assert.equal(callCount, 1);

  // Side effects should only happen once
  let sideEffectCount = 0;
  const init = once(function () {
    sideEffectCount += 1;
    return { initialized: true };
  });

  const obj1 = init();
  const obj2 = init();
  const obj3 = init();
  assert.equal(sideEffectCount, 1);
  assert.deepEqual(obj1, { initialized: true });
  // Same reference returned every time
  assert.equal(obj1, obj2);
  assert.equal(obj2, obj3);

  console.log('lodash.once-test:ok');
} catch (err) {
  console.error('lodash.once-test:fail');
  console.error(err && err.stack ? err.stack : String(err));
  process.exit(1);
}
