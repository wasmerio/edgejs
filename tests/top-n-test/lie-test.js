'use strict';

const assert = require('node:assert/strict');
const Lie = require('lie');

(async () => {
  // Basic resolve
  const resolved = await new Lie((resolve) => {
    resolve(42);
  });
  assert.equal(resolved, 42);

  // Basic reject
  try {
    await new Lie((_, reject) => {
      reject(new Error('oops'));
    });
    assert.fail('should have thrown');
  } catch (err) {
    assert.equal(err.message, 'oops');
  }

  // Then chaining
  const chained = await new Lie((resolve) => resolve(10))
    .then((val) => val * 2)
    .then((val) => val + 5);
  assert.equal(chained, 25);

  // Promise.all equivalent - Lie.all
  const results = await Lie.all([
    new Lie((resolve) => resolve(1)),
    new Lie((resolve) => resolve(2)),
    new Lie((resolve) => resolve(3)),
  ]);
  assert.deepEqual(results, [1, 2, 3]);

  // Promise.race equivalent - Lie.race
  const fastest = await Lie.race([
    new Lie((resolve) => setTimeout(() => resolve('slow'), 100)),
    new Lie((resolve) => resolve('fast')),
  ]);
  assert.equal(fastest, 'fast');

  // Verify it behaves as a thenable
  const lieInstance = new Lie((resolve) => resolve('hello'));
  assert.equal(typeof lieInstance.then, 'function');
  assert.equal(typeof lieInstance.catch, 'function');

  console.log('lie-test:ok');
})().catch((err) => {
  console.error('lie-test:fail');
  console.error(err && err.stack ? err.stack : String(err));
  process.exit(1);
});
