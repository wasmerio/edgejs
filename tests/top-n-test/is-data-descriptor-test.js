'use strict';

const assert = require('node:assert/strict');

(async () => {
  const isData = require('is-data-descriptor');

  assert.equal(typeof isData, 'function');

  // A data descriptor with value should be recognized
  const dataDesc = {
    value: 'hello',
    writable: true,
    enumerable: true,
    configurable: true
  };
  assert.equal(isData(dataDesc), true, '{value, writable} is a data descriptor');

  // A value-only descriptor is still a data descriptor
  const valueOnly = { value: 42 };
  assert.equal(isData(valueOnly), true, '{value} alone is a data descriptor');

  // value: undefined is still a data descriptor
  const undefinedValue = { value: undefined, writable: true };
  assert.equal(isData(undefinedValue), true, '{value: undefined, writable} is a data descriptor');

  // An accessor descriptor should NOT be a data descriptor
  const accessorDesc = {
    get: function() { return 'val'; },
    set: function(v) {},
    enumerable: true,
    configurable: true
  };
  assert.equal(isData(accessorDesc), false, '{get, set} is NOT a data descriptor');

  // A plain empty object is not a data descriptor
  assert.equal(isData({}), false, 'empty object is not a data descriptor');

  // Non-objects should return false
  assert.equal(isData(null), false, 'null is not a data descriptor');
  assert.equal(isData('string'), false, 'string is not a data descriptor');
  assert.equal(isData(123), false, 'number is not a data descriptor');

  // Test with a real data descriptor from defineProperty
  const obj = {};
  Object.defineProperty(obj, 'y', {
    value: 99,
    writable: false,
    enumerable: true,
    configurable: false
  });
  const realDesc = Object.getOwnPropertyDescriptor(obj, 'y');
  assert.equal(isData(realDesc), true, 'real data descriptor from Object.getOwnPropertyDescriptor');

  console.log('is-data-descriptor-test:ok');
})().catch((err) => {
  console.error('is-data-descriptor-test:fail');
  console.error(err && err.stack ? err.stack : String(err));
  process.exit(1);
});
