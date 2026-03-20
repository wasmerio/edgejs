'use strict';

const assert = require('node:assert/strict');

const isDateObject = require('is-date-object');

// True for actual Date objects
assert.equal(isDateObject(new Date()), true, 'new Date() should be a date object');
assert.equal(isDateObject(new Date('2020-01-01')), true, 'new Date with string arg should be a date');
assert.equal(isDateObject(new Date(0)), true, 'new Date(0) should be a date');

// False for non-Date values
assert.equal(isDateObject(Date.now()), false, 'Date.now() returns a number, not a Date');
assert.equal(isDateObject('2020-01-01'), false, 'date string is not a Date object');
assert.equal(isDateObject(1234567890), false, 'timestamp number is not a Date object');
assert.equal(isDateObject({}), false, 'plain object is not a Date object');
assert.equal(isDateObject([]), false, 'array is not a Date object');
assert.equal(isDateObject(null), false, 'null is not a Date object');
assert.equal(isDateObject(undefined), false, 'undefined is not a Date object');
assert.equal(isDateObject(true), false, 'boolean is not a Date object');
assert.equal(isDateObject(() => {}), false, 'function is not a Date object');
assert.equal(isDateObject({ toString: () => '[object Date]' }), false, 'spoofed toString should not fool it');

// Invalid date is still a Date object
assert.equal(isDateObject(new Date('invalid')), true, 'invalid Date is still a Date object');

console.log('is-date-object-test:ok');
