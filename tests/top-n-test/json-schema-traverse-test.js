'use strict';

const assert = require('node:assert/strict');

(async () => {
  const traverse = require('json-schema-traverse');

  // Traverse a schema with properties and collect visited JSON pointers
  const schema = {
    type: 'object',
    properties: {
      name: { type: 'string' },
      age: { type: 'integer' },
      address: {
        type: 'object',
        properties: {
          street: { type: 'string' },
          city: { type: 'string' },
        },
      },
    },
  };

  const visitedPaths = [];
  traverse(schema, (subSchema, jsonPtr) => {
    visitedPaths.push(jsonPtr);
  });

  // Root schema has empty string pointer
  assert.ok(visitedPaths.includes(''), 'should visit root schema');
  assert.ok(visitedPaths.includes('/properties/name'), 'should visit name property');
  assert.ok(visitedPaths.includes('/properties/age'), 'should visit age property');
  assert.ok(visitedPaths.includes('/properties/address'), 'should visit address property');
  assert.ok(visitedPaths.includes('/properties/address/properties/street'), 'should visit nested street');
  assert.ok(visitedPaths.includes('/properties/address/properties/city'), 'should visit nested city');

  // Schema with items (array schema)
  const arraySchema = {
    type: 'array',
    items: { type: 'string' },
  };

  const arrayPaths = [];
  traverse(arraySchema, (subSchema, jsonPtr) => {
    arrayPaths.push(jsonPtr);
  });

  assert.ok(arrayPaths.includes(''), 'should visit root');
  assert.ok(arrayPaths.includes('/items'), 'should visit items');

  // Schema with allOf
  const allOfSchema = {
    allOf: [
      { type: 'object', properties: { x: { type: 'number' } } },
      { type: 'object', properties: { y: { type: 'number' } } },
    ],
  };

  const allOfPaths = [];
  traverse(allOfSchema, (subSchema, jsonPtr) => {
    allOfPaths.push(jsonPtr);
  });

  assert.ok(allOfPaths.includes('/allOf/0'), 'should visit allOf[0]');
  assert.ok(allOfPaths.includes('/allOf/1'), 'should visit allOf[1]');
  assert.ok(allOfPaths.includes('/allOf/0/properties/x'), 'should visit allOf[0]/properties/x');
  assert.ok(allOfPaths.includes('/allOf/1/properties/y'), 'should visit allOf[1]/properties/y');

  // Pre and post callbacks using object form (third arg is the callback object)
  const preOrder = [];
  const postOrder = [];
  traverse(schema, {}, {
    pre: (subSchema, jsonPtr) => { preOrder.push(jsonPtr); },
    post: (subSchema, jsonPtr) => { postOrder.push(jsonPtr); },
  });

  // Pre visits root first, post visits root last
  assert.equal(preOrder[0], '');
  assert.equal(postOrder[postOrder.length - 1], '');
  // Both should visit the same set of paths
  assert.equal(preOrder.length, postOrder.length);

  // Callback receives correct arguments: rootSchema, parentJsonPtr, etc.
  let rootSchemaArg = null;
  let parentPtrSeen = null;
  traverse(schema, (subSchema, jsonPtr, rootSchema, parentJsonPtr, parentKeyword) => {
    if (jsonPtr === '/properties/name') {
      rootSchemaArg = rootSchema;
      parentPtrSeen = parentJsonPtr;
    }
  });
  assert.equal(rootSchemaArg, schema, 'rootSchema argument should be the original schema');
  assert.equal(parentPtrSeen, '', 'parent of /properties/name should be root');

  console.log('json-schema-traverse-test:ok');
})().catch((err) => {
  console.error('json-schema-traverse-test:fail');
  console.error(err && err.stack ? err.stack : String(err));
  process.exit(1);
});
