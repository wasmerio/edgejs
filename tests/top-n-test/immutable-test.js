'use strict';

const assert = require('node:assert/strict');

(async () => {
  const { Map, List, Set, fromJS, is } = require('immutable');

  // --- Map operations ---
  const map1 = Map({ a: 1, b: 2, c: 3 });
  assert.equal(map1.get('a'), 1);
  assert.equal(map1.get('b'), 2);
  assert.equal(map1.size, 3);

  // set returns a new Map, original is unchanged
  const map2 = map1.set('d', 4);
  assert.equal(map2.get('d'), 4);
  assert.equal(map2.size, 4);
  assert.equal(map1.size, 3, 'original map should be unchanged');

  // delete a key
  const map3 = map2.delete('a');
  assert.equal(map3.has('a'), false);
  assert.equal(map3.size, 3);

  // merge two maps
  const map4 = Map({ x: 10 });
  const merged = map1.merge(map4);
  assert.equal(merged.get('x'), 10);
  assert.equal(merged.get('a'), 1);

  // toJS converts back to plain object
  const plain = map1.toJS();
  assert.deepEqual(plain, { a: 1, b: 2, c: 3 });

  // Structural sharing: setting same value returns same reference
  const map5 = map1.set('a', 1);
  assert.ok(is(map1, map5), 'setting same value should return equivalent map');

  // --- List operations ---
  const list1 = List([1, 2, 3]);
  assert.equal(list1.get(0), 1);
  assert.equal(list1.size, 3);

  const list2 = list1.push(4);
  assert.equal(list2.size, 4);
  assert.equal(list2.get(3), 4);
  assert.equal(list1.size, 3, 'original list unchanged');

  const list3 = list1.set(1, 20);
  assert.equal(list3.get(1), 20);
  assert.equal(list1.get(1), 2, 'original list still has old value');

  // delete from list
  const list4 = list1.delete(0);
  assert.equal(list4.size, 2);
  assert.equal(list4.get(0), 2);

  // toJS
  assert.deepEqual(list1.toJS(), [1, 2, 3]);

  // --- Set operations ---
  const set1 = Set([1, 2, 3, 3, 2]);
  assert.equal(set1.size, 3, 'set should deduplicate');
  assert.ok(set1.has(1));
  assert.ok(set1.has(2));
  assert.ok(set1.has(3));

  const set2 = set1.add(4);
  assert.equal(set2.size, 4);
  assert.equal(set1.size, 3, 'original set unchanged');

  const set3 = set1.delete(2);
  assert.equal(set3.size, 2);
  assert.ok(!set3.has(2));

  // --- fromJS deep conversion ---
  const deep = fromJS({ users: [{ name: 'Alice' }, { name: 'Bob' }] });
  assert.equal(deep.getIn(['users', 0, 'name']), 'Alice');
  assert.equal(deep.getIn(['users', 1, 'name']), 'Bob');

  console.log('immutable-test:ok');
})().catch((err) => {
  console.error('immutable-test:fail');
  console.error(err && err.stack ? err.stack : String(err));
  process.exit(1);
});
