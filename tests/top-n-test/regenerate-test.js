'use strict';

const assert = require('node:assert/strict');

(async () => {
  const regenerate = require('regenerate');

  assert.equal(typeof regenerate, 'function');

  // Create a set and add individual code points
  const set = regenerate();
  set.add(0x61); // 'a'
  set.add(0x62); // 'b'
  set.add(0x63); // 'c'

  // Generate a regex string from the set
  const pattern = set.toString();
  assert.equal(typeof pattern, 'string');
  const re = new RegExp(pattern);
  assert.ok(re.test('a'), 'regex should match "a"');
  assert.ok(re.test('b'), 'regex should match "b"');
  assert.ok(!re.test('d'), 'regex should not match "d"');

  // Test adding a range of code points
  const digits = regenerate().addRange(0x30, 0x39); // '0'-'9'
  const digitPattern = digits.toString();
  const digitRe = new RegExp('^' + digitPattern + '+$');
  assert.ok(digitRe.test('0123456789'), 'should match all digits');
  assert.ok(!digitRe.test('abc'), 'should not match letters');

  // Test removing code points
  const vowels = regenerate(0x61, 0x65, 0x69, 0x6F, 0x75); // a, e, i, o, u
  vowels.remove(0x75); // remove 'u'
  const vowelPattern = vowels.toString();
  const vowelRe = new RegExp(vowelPattern);
  assert.ok(vowelRe.test('a'), 'should match a');
  assert.ok(vowelRe.test('e'), 'should match e');
  assert.ok(!vowelRe.test('u'), 'should not match u after removal');

  // Test creating from an array of code points
  const fromArray = regenerate([0x41, 0x42, 0x43]); // A, B, C
  const arrayPattern = fromArray.toString();
  const arrayRe = new RegExp(arrayPattern);
  assert.ok(arrayRe.test('A'), 'should match A');
  assert.ok(arrayRe.test('C'), 'should match C');
  assert.ok(!arrayRe.test('a'), 'should not match lowercase');

  // Test contains
  const containsSet = regenerate().addRange(0x61, 0x7A); // a-z
  assert.equal(containsSet.contains(0x61), true, 'should contain "a"');
  assert.equal(containsSet.contains(0x41), false, 'should not contain "A"');

  // Test toRegExp
  const reObj = regenerate(0x61, 0x62).toRegExp();
  assert.ok(reObj instanceof RegExp, 'toRegExp should return a RegExp');
  assert.ok(reObj.test('a'));

  console.log('regenerate-test:ok');
})().catch((err) => {
  console.error('regenerate-test:fail');
  console.error(err && err.stack ? err.stack : String(err));
  process.exit(1);
});
