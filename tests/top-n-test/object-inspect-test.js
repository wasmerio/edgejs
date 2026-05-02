'use strict';

const assert = require('node:assert/strict');
const inspect = require('object-inspect');

try {
  // Inspect primitives
  assert.equal(inspect(42), '42');
  assert.equal(inspect('hello'), "'hello'");
  assert.equal(inspect(true), 'true');
  assert.equal(inspect(null), 'null');
  assert.equal(inspect(undefined), 'undefined');

  // Inspect arrays
  assert.equal(inspect([1, 2, 3]), '[ 1, 2, 3 ]');
  assert.equal(inspect([]), '[]');

  // Inspect objects
  const objStr = inspect({ a: 1, b: 2 });
  assert.ok(objStr.includes('a'), 'should contain key a');
  assert.ok(objStr.includes('1'), 'should contain value 1');
  assert.ok(objStr.includes('b'), 'should contain key b');

  // Inspect nested structures
  const nestedStr = inspect({ x: { y: [1, 2] } });
  assert.ok(nestedStr.includes('x'), 'should show nested key x');
  assert.ok(nestedStr.includes('y'), 'should show nested key y');

  // Inspect functions
  function myFunc() {}
  const funcStr = inspect(myFunc);
  assert.ok(funcStr.includes('myFunc'), 'should include function name');

  // Inspect symbols
  const sym = Symbol('test');
  const symStr = inspect(sym);
  assert.ok(symStr.includes('test'), 'should include symbol description');

  // Inspect regex
  const regStr = inspect(/foo/gi);
  assert.ok(regStr.includes('foo'), 'should include regex pattern');

  // Inspect Date
  const d = new Date('2024-01-15T00:00:00.000Z');
  const dateStr = inspect(d);
  assert.ok(dateStr.includes('2024'), 'should include year in date string');

  // Inspect with depth option
  const deep = { a: { b: { c: { d: 1 } } } };
  const shallowStr = inspect(deep, { depth: 1 });
  assert.equal(typeof shallowStr, 'string');
  assert.ok(shallowStr.length > 0);

  console.log('object-inspect-test:ok');
} catch (err) {
  console.error('object-inspect-test:fail');
  console.error(err && err.stack ? err.stack : String(err));
  process.exit(1);
}
