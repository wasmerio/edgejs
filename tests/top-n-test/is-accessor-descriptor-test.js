'use strict';

const assert = require('node:assert/strict');

(async () => {
  const isAccessor = require('is-accessor-descriptor');

  assert.equal(typeof isAccessor, 'function');

  // A getter/setter descriptor should be recognized as an accessor
  const accessorDesc = {
    get: function() { return 'value'; },
    set: function(v) {},
    enumerable: true,
    configurable: true
  };
  assert.equal(isAccessor(accessorDesc), true, '{get, set} is an accessor descriptor');

  // A setter-only descriptor is also an accessor
  const setterOnly = {
    get: function() { return 42; },
    set: function(v) {},
    enumerable: false,
    configurable: true
  };
  assert.equal(isAccessor(setterOnly), true, '{get, set} with different enumerable is an accessor');

  // A data descriptor (with value) should NOT be an accessor
  const dataDesc = {
    value: 'hello',
    writable: true,
    enumerable: true,
    configurable: true
  };
  assert.equal(isAccessor(dataDesc), false, '{value, writable} is NOT an accessor descriptor');

  // A plain object should NOT be an accessor
  assert.equal(isAccessor({ foo: 'bar' }), false, 'plain object is not an accessor descriptor');

  // Non-objects should return false
  assert.equal(isAccessor(null), false, 'null is not an accessor descriptor');
  assert.equal(isAccessor('string'), false, 'string is not an accessor descriptor');
  assert.equal(isAccessor(42), false, 'number is not an accessor descriptor');

  // Test with an actual object property descriptor
  const obj = {};
  Object.defineProperty(obj, 'x', {
    get() { return 10; },
    enumerable: true,
    configurable: true
  });
  const realDesc = Object.getOwnPropertyDescriptor(obj, 'x');
  assert.equal(isAccessor(realDesc), true, 'real accessor descriptor from Object.getOwnPropertyDescriptor');

  console.log('is-accessor-descriptor-test:ok');
})().catch((err) => {
  console.error('is-accessor-descriptor-test:fail');
  console.error(err && err.stack ? err.stack : String(err));
  process.exit(1);
});
