'use strict';

const assert = require('node:assert/strict');
const GetIntrinsic = require('get-intrinsic');

try {
  // Get Array.prototype.push and verify it works
  const push = GetIntrinsic('%Array.prototype.push%');
  assert.equal(typeof push, 'function');
  const arr = [1, 2];
  push.call(arr, 3);
  assert.deepEqual(arr, [1, 2, 3]);

  // Get Object.prototype.toString and verify it works
  const toString = GetIntrinsic('%Object.prototype.toString%');
  assert.equal(typeof toString, 'function');
  assert.equal(toString.call([]), '[object Array]');
  assert.equal(toString.call({}), '[object Object]');

  // Get Math.pow and verify it works
  const pow = GetIntrinsic('%Math.pow%');
  assert.equal(typeof pow, 'function');
  assert.equal(pow(2, 10), 1024);
  assert.equal(pow(3, 3), 27);

  // Get JSON.parse
  const parse = GetIntrinsic('%JSON.parse%');
  assert.equal(typeof parse, 'function');
  assert.deepEqual(parse('{"a":1}'), { a: 1 });

  // Requesting a non-existent property with allowMissing should return undefined
  const missing = GetIntrinsic('%Array.prototype.nonExistentMethod12345%', true);
  assert.equal(missing, undefined);

  // Requesting a non-existent intrinsic without allowMissing should throw
  assert.throws(() => {
    GetIntrinsic('%Array.prototype.nonExistentMethod12345%');
  }, 'should throw for unknown intrinsic');

  console.log('get-intrinsic-test:ok');
} catch (err) {
  console.error('get-intrinsic-test:fail');
  console.error(err && err.stack ? err.stack : String(err));
  process.exit(1);
}
