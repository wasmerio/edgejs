'use strict';

require('../common');
const assert = require('assert');
const events = require('events');

const E = events.EventEmitter.prototype;
assert.strictEqual(E.constructor.name, 'EventEmitter');
assert.strictEqual(E.on, E.addListener);
assert.strictEqual(E.off, E.removeListener);

for (const name of Object.getOwnPropertyNames(E)) {
  if (name === 'constructor' || name === 'on' || name === 'off') continue;
  if (typeof E[name] !== 'function') continue;
  assert.strictEqual(E[name].name, name);
}
