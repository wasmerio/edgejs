'use strict';

const assert = require('node:assert/strict');
const { camelCase } = require('camel-case');

try {
  // Kebab-case to camelCase
  assert.equal(camelCase('foo-bar'), 'fooBar');

  // Snake_case to camelCase
  assert.equal(camelCase('foo_bar'), 'fooBar');

  // PascalCase to camelCase
  assert.equal(camelCase('FooBar'), 'fooBar');

  // SCREAMING_SNAKE_CASE to camelCase
  assert.equal(camelCase('FOO_BAR'), 'fooBar');

  // Multi-word strings
  assert.equal(camelCase('hello world'), 'helloWorld');
  assert.equal(camelCase('some-long-variable-name'), 'someLongVariableName');

  // Single word stays lowercase
  assert.equal(camelCase('foo'), 'foo');

  console.log('camel-case-test:ok');
} catch (err) {
  console.error('camel-case-test:fail');
  console.error(err && err.stack ? err.stack : String(err));
  process.exit(1);
}
