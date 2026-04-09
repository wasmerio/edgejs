'use strict';

const assert = require('node:assert/strict');

try {
  const setPrototypeOf = require('setprototypeof');

  // Basic prototype setting
  const proto = { greet() { return 'hello'; } };
  const obj = {};
  setPrototypeOf(obj, proto);
  assert.equal(obj.greet(), 'hello');

  // Verify the prototype chain with Object.getPrototypeOf
  assert.equal(Object.getPrototypeOf(obj), proto);

  // instanceof works after setting prototype
  function Animal() {}
  Animal.prototype.speak = function () { return 'generic sound'; };
  const thing = {};
  setPrototypeOf(thing, Animal.prototype);
  assert.ok(thing instanceof Animal);
  assert.equal(thing.speak(), 'generic sound');

  // Prototype chain supports inheritance depth
  function Base() {}
  Base.prototype.baseMethod = function () { return 'base'; };
  function Child() {}
  Child.prototype = Object.create(Base.prototype);
  Child.prototype.childMethod = function () { return 'child'; };

  const instance = {};
  setPrototypeOf(instance, Child.prototype);
  assert.ok(instance instanceof Child);
  assert.ok(instance instanceof Base);
  assert.equal(instance.childMethod(), 'child');
  assert.equal(instance.baseMethod(), 'base');

  // Overriding an existing prototype
  const protoA = { name: 'A' };
  const protoB = { name: 'B' };
  const target = {};
  setPrototypeOf(target, protoA);
  assert.equal(target.name, 'A');
  setPrototypeOf(target, protoB);
  assert.equal(target.name, 'B');

  // Works with constructor functions as targets
  function CustomError(msg) { this.message = msg; }
  setPrototypeOf(CustomError.prototype, Error.prototype);
  const err = new CustomError('test');
  assert.ok(err instanceof CustomError);
  assert.ok(err instanceof Error);
  assert.equal(err.message, 'test');

  console.log('setprototypeof-test:ok');
} catch (err) {
  console.error('setprototypeof-test:fail');
  console.error(err && err.stack ? err.stack : String(err));
  process.exit(1);
}
