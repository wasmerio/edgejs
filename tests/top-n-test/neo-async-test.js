'use strict';

const assert = require('node:assert/strict');

const async = require('neo-async');

// Test each: iterate over an array
(function testEach() {
  const collected = [];
  async.each([1, 2, 3], (item, cb) => {
    collected.push(item);
    cb(null);
  }, (err) => {
    assert.equal(err, null);
    assert.deepEqual(collected.sort(), [1, 2, 3]);
  });
})();

// Test map: transform an array
(function testMap() {
  async.map([10, 20, 30], (item, cb) => {
    cb(null, item * 2);
  }, (err, results) => {
    assert.equal(err, null);
    assert.deepEqual(results, [20, 40, 60]);
  });
})();

// Test parallel: run tasks in parallel
(function testParallel() {
  async.parallel([
    (cb) => cb(null, 'one'),
    (cb) => cb(null, 'two'),
    (cb) => cb(null, 'three'),
  ], (err, results) => {
    assert.equal(err, null);
    assert.deepEqual(results, ['one', 'two', 'three']);
  });
})();

// Test series: run tasks in sequence
(function testSeries() {
  const order = [];
  async.series([
    (cb) => { order.push('a'); cb(null, 'first'); },
    (cb) => { order.push('b'); cb(null, 'second'); },
  ], (err, results) => {
    assert.equal(err, null);
    assert.deepEqual(results, ['first', 'second']);
    assert.deepEqual(order, ['a', 'b']);
  });
})();

// Test waterfall: pass results from one task to the next
(function testWaterfall() {
  async.waterfall([
    (cb) => cb(null, 5),
    (val, cb) => cb(null, val + 10),
    (val, cb) => cb(null, val * 2),
  ], (err, result) => {
    assert.equal(err, null);
    assert.equal(result, 30);
  });
})();

console.log('neo-async-test:ok');
