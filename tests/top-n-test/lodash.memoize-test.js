'use strict';

const assert = require('node:assert/strict');

(async () => {
  const memoize = require('lodash.memoize');

  assert.equal(typeof memoize, 'function');

  // Memoize a simple computation
  let computeCount = 0;
  const square = memoize((n) => {
    computeCount++;
    return n * n;
  });

  assert.equal(square(4), 16);
  assert.equal(computeCount, 1, 'first call should compute');

  assert.equal(square(4), 16);
  assert.equal(computeCount, 1, 'second call with same arg should use cache');

  assert.equal(square(5), 25);
  assert.equal(computeCount, 2, 'different argument should trigger new computation');

  // Verify the cache property exists and works
  assert.ok(square.cache instanceof Map || typeof square.cache === 'object',
    'memoized function should have a cache property');
  assert.equal(square.cache.get(4), 16);
  assert.equal(square.cache.get(5), 25);

  // Test with a custom resolver
  let resolverCount = 0;
  const greet = memoize(
    (first, last) => {
      resolverCount++;
      return `Hello ${first} ${last}`;
    },
    (first, last) => `${first}-${last}`
  );

  assert.equal(greet('John', 'Doe'), 'Hello John Doe');
  assert.equal(resolverCount, 1);

  assert.equal(greet('John', 'Doe'), 'Hello John Doe');
  assert.equal(resolverCount, 1, 'same args should be cached via resolver key');

  assert.equal(greet('Jane', 'Doe'), 'Hello Jane Doe');
  assert.equal(resolverCount, 2, 'different args should trigger new call');

  // Verify cache uses the resolver key
  assert.equal(greet.cache.get('John-Doe'), 'Hello John Doe');

  // Test clearing the cache
  square.cache.clear();
  computeCount = 0;
  assert.equal(square(4), 16);
  assert.equal(computeCount, 1, 'should recompute after cache clear');

  console.log('lodash.memoize-test:ok');
})().catch((err) => {
  console.error('lodash.memoize-test:fail');
  console.error(err && err.stack ? err.stack : String(err));
  process.exit(1);
});
