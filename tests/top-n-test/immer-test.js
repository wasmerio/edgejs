'use strict';

const assert = require('node:assert/strict');
const { produce } = require('immer');

try {
  // Basic produce: update a property immutably
  const baseState = { name: 'Alice', age: 30 };
  const nextState = produce(baseState, (draft) => {
    draft.age = 31;
  });

  assert.equal(nextState.age, 31, 'new state should have updated age');
  assert.equal(baseState.age, 30, 'original state should be unchanged');
  assert.notEqual(baseState, nextState, 'should return a new object');
  assert.equal(nextState.name, 'Alice', 'untouched properties should carry over');

  // Nested updates
  const nested = { user: { name: 'Bob', address: { city: 'NYC' } }, tags: ['a', 'b'] };
  const updated = produce(nested, (draft) => {
    draft.user.address.city = 'LA';
  });

  assert.equal(updated.user.address.city, 'LA');
  assert.equal(nested.user.address.city, 'NYC', 'original nested value unchanged');
  assert.equal(updated.tags, nested.tags, 'unmodified subtrees share references');

  // Array push
  const withArray = { items: [1, 2, 3] };
  const pushed = produce(withArray, (draft) => {
    draft.items.push(4);
  });

  assert.deepEqual(pushed.items, [1, 2, 3, 4]);
  assert.deepEqual(withArray.items, [1, 2, 3], 'original array unchanged');

  // No-op produce returns same reference
  const same = produce(baseState, () => {
    // no changes
  });
  assert.equal(same, baseState, 'no-op should return the same reference');

  console.log('immer-test:ok');
} catch (err) {
  console.error('immer-test:fail');
  console.error(err && err.stack ? err.stack : String(err));
  process.exit(1);
}
