'use strict';

const assert = require('node:assert/strict');

const tsNode = require('ts-node');

// Verify register function exists
assert.equal(typeof tsNode.register, 'function', 'should have a register function');

// Verify create function exists
assert.equal(typeof tsNode.create, 'function', 'should have a create function');

// Check VERSION is a string
assert.equal(typeof tsNode.VERSION, 'string', 'should expose VERSION');
assert.ok(tsNode.VERSION.length > 0, 'VERSION should not be empty');

// Verify createRepl exists
assert.equal(typeof tsNode.createRepl, 'function', 'should have createRepl');

// If typescript is available, test compilation
let hasTypescript = true;
try {
  require.resolve('typescript');
} catch (_) {
  hasTypescript = false;
}

if (hasTypescript) {
  const service = tsNode.create({
    transpileOnly: true,
    compilerOptions: {
      module: 'Node16',
      target: 'es2019',
      moduleResolution: 'node16',
      ignoreDeprecations: '6.0',
    },
  });
  assert.equal(typeof service.compile, 'function', 'service should have compile');

  const output = service.compile('const greeting: string = "hello";', 'test.ts');
  assert.equal(typeof output, 'string', 'compile should return a string');
  assert.ok(output.includes('hello'), 'compiled output should contain the string literal');
}

console.log('ts-node-test:ok');
