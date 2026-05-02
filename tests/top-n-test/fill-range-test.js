'use strict';

const assert = require('node:assert/strict');
const fill = require('fill-range');

try {
  // Fill numeric range 1 to 5
  assert.deepEqual(fill(1, 5), [1, 2, 3, 4, 5]);

  // Fill alpha range a to e
  assert.deepEqual(fill('a', 'e'), ['a', 'b', 'c', 'd', 'e']);

  // Fill with a step of 2
  assert.deepEqual(fill(1, 10, 2), [1, 3, 5, 7, 9]);

  // Fill with a transform function
  const result = fill(1, 5, (val) => 'item-' + val);
  assert.deepEqual(result, ['item-1', 'item-2', 'item-3', 'item-4', 'item-5']);

  // Zero-padding
  assert.deepEqual(fill('01', '05'), ['01', '02', '03', '04', '05']);

  // Descending range
  assert.deepEqual(fill(5, 1), [5, 4, 3, 2, 1]);

  // Alpha descending
  assert.deepEqual(fill('e', 'a'), ['e', 'd', 'c', 'b', 'a']);

  console.log('fill-range-test:ok');
} catch (err) {
  console.error('fill-range-test:fail');
  console.error(err && err.stack ? err.stack : String(err));
  process.exit(1);
}
