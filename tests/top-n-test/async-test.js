'use strict';

const assert = require('node:assert/strict');
const async = require('async');

(async () => {
  // async.map - transform an array with an async iterator
  const mapped = await async.map([1, 2, 3], (item, cb) => {
    setTimeout(() => cb(null, item * 2), 5);
  });
  assert.deepEqual(mapped, [2, 4, 6]);

  // async.parallel - run tasks in parallel, collect results
  const parallelResult = await async.parallel({
    one: (cb) => setTimeout(() => cb(null, 'first'), 5),
    two: (cb) => setTimeout(() => cb(null, 'second'), 5),
  });
  assert.equal(parallelResult.one, 'first');
  assert.equal(parallelResult.two, 'second');

  // async.parallel with array
  const parallelArr = await async.parallel([
    (cb) => cb(null, 10),
    (cb) => cb(null, 20),
    (cb) => cb(null, 30),
  ]);
  assert.deepEqual(parallelArr, [10, 20, 30]);

  // async.series - run tasks sequentially
  const order = [];
  const seriesResult = await async.series([
    (cb) => { order.push('a'); cb(null, 'A'); },
    (cb) => { order.push('b'); cb(null, 'B'); },
    (cb) => { order.push('c'); cb(null, 'C'); },
  ]);
  assert.deepEqual(seriesResult, ['A', 'B', 'C']);
  assert.deepEqual(order, ['a', 'b', 'c']);

  // async.waterfall - pass results from one task to the next
  const waterfallResult = await async.waterfall([
    (cb) => cb(null, 5),
    (val, cb) => cb(null, val * 3),
    (val, cb) => cb(null, val + 1),
  ]);
  assert.equal(waterfallResult, 16); // 5 * 3 + 1

  // async.each - iterate over items with an async function
  const collected = [];
  await async.each([10, 20, 30], (item, cb) => {
    collected.push(item);
    cb(null);
  });
  // each runs in parallel, so sort to compare
  collected.sort((a, b) => a - b);
  assert.deepEqual(collected, [10, 20, 30]);

  // async.eachSeries - iterate sequentially
  const seqCollected = [];
  await async.eachSeries(['x', 'y', 'z'], (item, cb) => {
    seqCollected.push(item);
    cb(null);
  });
  assert.deepEqual(seqCollected, ['x', 'y', 'z']);

  console.log('async-test:ok');
})().catch((err) => {
  console.error('async-test:fail');
  console.error(err && err.stack ? err.stack : String(err));
  process.exit(1);
});
