'use strict';

const assert = require('node:assert/strict');

(async () => {
  const Ajv = require('ajv');

  // Create an Ajv instance and compile a basic schema
  const ajv = new Ajv();
  const schema = {
    type: 'object',
    properties: {
      name: { type: 'string', minLength: 1 },
      age: { type: 'integer', minimum: 0 },
      email: { type: 'string' },
    },
    required: ['name', 'age'],
    additionalProperties: false,
  };

  const validate = ajv.compile(schema);

  // Valid data passes
  assert.equal(validate({ name: 'Alice', age: 30 }), true);
  assert.equal(validate({ name: 'Bob', age: 25, email: 'bob@test.com' }), true);
  assert.equal(validate.errors, null);

  // Missing required field fails
  assert.equal(validate({ name: 'Alice' }), false);
  assert.ok(Array.isArray(validate.errors));
  assert.ok(validate.errors.length > 0);
  assert.equal(validate.errors[0].keyword, 'required');

  // Wrong type fails
  assert.equal(validate({ name: 'Alice', age: 'thirty' }), false);
  assert.ok(validate.errors.some(e => e.keyword === 'type'));

  // Additional properties are rejected
  assert.equal(validate({ name: 'Alice', age: 30, role: 'admin' }), false);
  assert.ok(validate.errors.some(e => e.keyword === 'additionalProperties'));

  // minLength constraint
  assert.equal(validate({ name: '', age: 30 }), false);
  assert.ok(validate.errors.some(e => e.keyword === 'minLength'));

  // minimum constraint
  assert.equal(validate({ name: 'Alice', age: -1 }), false);
  assert.ok(validate.errors.some(e => e.keyword === 'minimum'));

  // Compile a schema with nested objects and arrays
  const complexSchema = {
    type: 'object',
    properties: {
      tags: {
        type: 'array',
        items: { type: 'string' },
        minItems: 1,
      },
      address: {
        type: 'object',
        properties: {
          street: { type: 'string' },
          zip: { type: 'string', pattern: '^[0-9]{5}$' },
        },
        required: ['street'],
      },
    },
  };

  const validateComplex = ajv.compile(complexSchema);
  assert.equal(validateComplex({ tags: ['a', 'b'], address: { street: '123 Main' } }), true);
  assert.equal(validateComplex({ tags: [] }), false); // minItems
  assert.equal(validateComplex({ tags: [123] }), false); // items type
  assert.equal(validateComplex({ address: { street: '123 Main', zip: 'abc' } }), false); // pattern

  // Use addSchema / getSchema pattern
  ajv.addSchema({ type: 'string', minLength: 2 }, 'shortString');
  const shortStringValidate = ajv.getSchema('shortString');
  assert.equal(typeof shortStringValidate, 'function');
  assert.equal(shortStringValidate('hi'), true);
  assert.equal(shortStringValidate('x'), false);

  console.log('ajv-test:ok');
})().catch((err) => {
  console.error('ajv-test:fail');
  console.error(err && err.stack ? err.stack : String(err));
  process.exit(1);
});
