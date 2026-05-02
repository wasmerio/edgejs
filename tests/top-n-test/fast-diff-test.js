'use strict';

const assert = require('node:assert/strict');
const diff = require('fast-diff');

try {
  // Diff constants
  const EQUAL = 0;
  const INSERT = 1;
  const DELETE = -1;

  // Identical strings should produce a single EQUAL tuple
  const same = diff('hello', 'hello');
  assert.equal(same.length, 1);
  assert.deepEqual(same[0], [EQUAL, 'hello']);

  // Insertion: adding text
  const insertion = diff('abc', 'abcdef');
  // Should contain the original text as EQUAL and the added part as INSERT
  const insertedText = insertion
    .filter(([op]) => op === INSERT)
    .map(([, text]) => text)
    .join('');
  assert.equal(insertedText, 'def');

  // Deletion: removing text
  const deletion = diff('abcdef', 'abc');
  const deletedText = deletion
    .filter(([op]) => op === DELETE)
    .map(([, text]) => text)
    .join('');
  assert.equal(deletedText, 'def');

  // Modification: replacing text
  const modification = diff('cat', 'hat');
  // Should have some combination of delete/insert for the changed part
  const hasDelete = modification.some(([op]) => op === DELETE);
  const hasInsert = modification.some(([op]) => op === INSERT);
  assert.ok(hasDelete, 'modification should have a delete');
  assert.ok(hasInsert, 'modification should have an insert');

  // Verify diff tuple format: each element is [number, string]
  const result = diff('foo bar', 'foo baz');
  for (const tuple of result) {
    assert.ok(Array.isArray(tuple), 'each diff entry should be an array');
    assert.equal(tuple.length, 2, 'each diff tuple should have 2 elements');
    assert.equal(typeof tuple[0], 'number', 'first element should be operation code');
    assert.equal(typeof tuple[1], 'string', 'second element should be text');
    assert.ok([DELETE, EQUAL, INSERT].includes(tuple[0]), 'operation must be -1, 0, or 1');
  }

  // Empty to non-empty is a full insertion
  const fromEmpty = diff('', 'hello');
  assert.deepEqual(fromEmpty, [[INSERT, 'hello']]);

  // Non-empty to empty is a full deletion
  const toEmpty = diff('hello', '');
  assert.deepEqual(toEmpty, [[DELETE, 'hello']]);

  console.log('fast-diff-test:ok');
} catch (err) {
  console.error('fast-diff-test:fail');
  console.error(err && err.stack ? err.stack : String(err));
  process.exit(1);
}
