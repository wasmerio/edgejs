'use strict';

const assert = require('node:assert/strict');

(async () => {
  const uuid = require('uuid');

  // Generate a v4 UUID
  const id1 = uuid.v4();
  assert.equal(typeof id1, 'string');
  assert.match(id1, /^[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$/i,
    'v4 UUID should match the standard format with version 4 marker');

  // Each call produces a unique UUID
  const id2 = uuid.v4();
  assert.notEqual(id1, id2, 'two v4 UUIDs should be different');

  // validate checks if a string is a valid UUID
  assert.equal(uuid.validate(id1), true);
  assert.equal(uuid.validate(id2), true);
  assert.equal(uuid.validate('not-a-uuid'), false);
  assert.equal(uuid.validate(''), false);
  assert.equal(uuid.validate('12345678-1234-1234-1234-123456789012'), false,
    'validate should reject UUIDs with incorrect version/variant bits');

  // version extracts the version number from a UUID
  assert.equal(uuid.version(id1), 4);

  // v1 UUID (timestamp-based) if available
  if (typeof uuid.v1 === 'function') {
    const v1id = uuid.v1();
    assert.equal(typeof v1id, 'string');
    assert.equal(uuid.validate(v1id), true);
    assert.equal(uuid.version(v1id), 1);

    // v1 UUIDs are also unique
    const v1id2 = uuid.v1();
    assert.notEqual(v1id, v1id2);
  }

  // NIL UUID (all zeros)
  assert.equal(uuid.NIL, '00000000-0000-0000-0000-000000000000');
  assert.equal(uuid.validate(uuid.NIL), true);

  // v3 (name-based MD5) and v5 (name-based SHA-1) if available
  if (typeof uuid.v5 === 'function') {
    // v5 with DNS namespace
    const v5id = uuid.v5('example.com', uuid.v5.DNS);
    assert.equal(uuid.validate(v5id), true);
    assert.equal(uuid.version(v5id), 5);

    // Same input always produces same output (deterministic)
    const v5id2 = uuid.v5('example.com', uuid.v5.DNS);
    assert.equal(v5id, v5id2, 'v5 should be deterministic');

    // Different input produces different output
    const v5id3 = uuid.v5('other.com', uuid.v5.DNS);
    assert.notEqual(v5id, v5id3);
  }

  if (typeof uuid.v3 === 'function') {
    const v3id = uuid.v3('example.com', uuid.v3.DNS);
    assert.equal(uuid.validate(v3id), true);
    assert.equal(uuid.version(v3id), 3);
  }

  // stringify and parse for converting between bytes and string
  if (typeof uuid.parse === 'function' && typeof uuid.stringify === 'function') {
    const bytes = uuid.parse(id1);
    assert.equal(bytes.length, 16);
    const backToString = uuid.stringify(bytes);
    assert.equal(backToString, id1);
  }

  console.log('uuid-test:ok');
})().catch((err) => {
  console.error('uuid-test:fail');
  console.error(err && err.stack ? err.stack : String(err));
  process.exit(1);
});
