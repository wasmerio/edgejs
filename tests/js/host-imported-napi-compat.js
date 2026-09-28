'use strict';

// Run with the WASIX guest against a shared host N-API provider. These
// assertions cover APIs whose persistent native buffers are intentionally
// unavailable to guests.
const assert = require('node:assert/strict');
const vm = require('node:vm');
const v8 = require('node:v8');
const test = require('node:test');

assert.equal(typeof WebAssembly, 'undefined');
assert.equal(vm.runInNewContext('typeof WebAssembly'), 'undefined');
assert.equal(typeof v8.getHeapStatistics, 'function');
assert.equal(typeof test, 'function');
assert.throws(() => new v8.Serializer(), /serialization is unavailable/);
assert.throws(() => new v8.Deserializer(Buffer.alloc(0)), /deserialization is unavailable/);
assert.throws(() => v8.serialize({ value: 1 }), /serialization is unavailable/);
assert.throws(() => v8.deserialize(Buffer.alloc(0)), /deserialization is unavailable/);

for (const source of [')', '(', 'return 1']) {
  assert.throws(() => new vm.Script(source), SyntaxError);
}

globalThis.edgeScriptSideEffect = 0;
const script = new vm.Script('globalThis.edgeScriptSideEffect += 1; 42', {
  cachedData: Buffer.from([1, 2, 3]),
  produceCachedData: true,
});
assert.equal(globalThis.edgeScriptSideEffect, 0, 'syntax check must not execute the script');
assert.equal(script.cachedDataRejected, true);
assert.equal(script.cachedDataProduced, false);
assert.throws(() => script.createCachedData(), /cached data is unavailable/);
assert.equal(script.runInThisContext(), 42);
assert.equal(globalThis.edgeScriptSideEffect, 1);

const fn = vm.compileFunction('return value + 1', ['value'], {
  cachedData: Buffer.from([1, 2, 3]),
  produceCachedData: true,
});
assert.equal(fn(41), 42);
assert.equal(fn.cachedDataRejected, true);
assert.equal(fn.cachedDataProduced, false);

console.log('EDGEJS_HOST_NAPI_COMPAT_OK');
