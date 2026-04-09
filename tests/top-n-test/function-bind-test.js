'use strict';

const assert = require('node:assert/strict');
const bind = require('function-bind');

try {
  // Bind context (this binding)
  function greet(greeting) {
    return greeting + ', ' + this.name;
  }
  const boundGreet = bind.call(greet, { name: 'Alice' });
  assert.equal(boundGreet('Hello'), 'Hello, Alice');

  // Bind with partial arguments
  function add(a, b) {
    return this.base + a + b;
  }
  const addFrom10 = bind.call(add, { base: 10 }, 3);
  assert.equal(addFrom10(7), 20); // 10 + 3 + 7

  // Multiple partial args
  function list() {
    return Array.prototype.slice.call(arguments);
  }
  const listFrom = bind.call(list, null, 'a', 'b');
  assert.deepEqual(listFrom('c', 'd'), ['a', 'b', 'c', 'd']);

  // Bind used as constructor - bound functions can be used with new
  function Point(x, y) {
    this.x = x;
    this.y = y;
  }
  const BoundPoint = bind.call(Point, null, 5);
  const pt = new BoundPoint(10);
  assert.equal(pt.x, 5);
  assert.equal(pt.y, 10);
  assert.ok(pt instanceof Point, 'instance should be instanceof original constructor');

  // Verify this binding is correctly applied
  const obj = { value: 99 };
  function getValue() {
    return this.value;
  }
  const boundGetValue = bind.call(getValue, obj);
  assert.equal(boundGetValue(), 99);

  console.log('function-bind-test:ok');
} catch (err) {
  console.error('function-bind-test:fail');
  console.error(err && err.stack ? err.stack : String(err));
  process.exit(1);
}
