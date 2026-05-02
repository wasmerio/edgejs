'use strict';

const assert = require('node:assert/strict');
const YAML = require('yaml');

try {
  // Parse a simple YAML string to an object
  const simple = YAML.parse('name: Alice\nage: 30');
  assert.deepEqual(simple, { name: 'Alice', age: 30 });

  // Stringify an object back to YAML
  const str = YAML.stringify({ greeting: 'hello', count: 42 });
  assert.equal(typeof str, 'string');
  const roundtrip = YAML.parse(str);
  assert.equal(roundtrip.greeting, 'hello');
  assert.equal(roundtrip.count, 42);

  // Handle arrays
  const arrYaml = 'fruits:\n  - apple\n  - banana\n  - cherry';
  const arrResult = YAML.parse(arrYaml);
  assert.ok(Array.isArray(arrResult.fruits));
  assert.equal(arrResult.fruits.length, 3);
  assert.equal(arrResult.fruits[0], 'apple');
  assert.equal(arrResult.fruits[2], 'cherry');

  // Handle nested objects
  const nested = YAML.parse('server:\n  host: localhost\n  port: 8080\n  ssl:\n    enabled: true');
  assert.equal(nested.server.host, 'localhost');
  assert.equal(nested.server.port, 8080);
  assert.equal(nested.server.ssl.enabled, true);

  // Stringify nested structures and verify round-trip
  const complexObj = {
    users: [
      { name: 'Bob', roles: ['admin', 'user'] },
      { name: 'Eve', roles: ['user'] },
    ],
  };
  const complexYaml = YAML.stringify(complexObj);
  const parsed = YAML.parse(complexYaml);
  assert.equal(parsed.users.length, 2);
  assert.equal(parsed.users[0].name, 'Bob');
  assert.deepEqual(parsed.users[0].roles, ['admin', 'user']);

  // Multi-document parsing with parseAllDocuments
  const multiDoc = '---\nfoo: 1\n---\nbar: 2\n';
  const docs = YAML.parseAllDocuments(multiDoc);
  assert.equal(docs.length, 2);
  assert.equal(docs[0].toJSON().foo, 1);
  assert.equal(docs[1].toJSON().bar, 2);

  // Parse null/boolean/number YAML scalars
  assert.equal(YAML.parse('true'), true);
  assert.equal(YAML.parse('42'), 42);
  assert.equal(YAML.parse('null'), null);
  assert.equal(YAML.parse('3.14'), 3.14);

  console.log('yaml-test:ok');
} catch (err) {
  console.error('yaml-test:fail');
  console.error(err && err.stack ? err.stack : String(err));
  process.exit(1);
}
