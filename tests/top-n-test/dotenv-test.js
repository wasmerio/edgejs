'use strict';

const assert = require('node:assert/strict');

try {
  const dotenv = require('dotenv');

  // Parse a simple dotenv string with key-value pairs
  const basic = dotenv.parse('DB_HOST=localhost\nDB_PORT=5432\nDB_NAME=myapp');
  assert.equal(basic.DB_HOST, 'localhost');
  assert.equal(basic.DB_PORT, '5432');
  assert.equal(basic.DB_NAME, 'myapp');

  // Comments should be ignored
  const withComments = dotenv.parse('# this is a comment\nAPI_KEY=secret123\n# another comment\nDEBUG=true');
  assert.equal(withComments.API_KEY, 'secret123');
  assert.equal(withComments.DEBUG, 'true');
  assert.equal(withComments['# this is a comment'], undefined);

  // Double-quoted values should strip quotes
  const doubleQuoted = dotenv.parse('GREETING="hello world"');
  assert.equal(doubleQuoted.GREETING, 'hello world');

  // Single-quoted values should strip quotes
  const singleQuoted = dotenv.parse("MESSAGE='no interpolation here'");
  assert.equal(singleQuoted.MESSAGE, 'no interpolation here');

  // Empty values
  const emptyVal = dotenv.parse('EMPTY=\nALSO_SET=yes');
  assert.equal(emptyVal.EMPTY, '');
  assert.equal(emptyVal.ALSO_SET, 'yes');

  // Multiline values in double quotes
  const multiline = dotenv.parse('CERT="line1\nline2\nline3"');
  assert.ok(multiline.CERT.includes('line1'));
  assert.ok(multiline.CERT.includes('line3'));

  // Values with equals sign
  const withEquals = dotenv.parse('DATABASE_URL=postgres://user:pass@host:5432/db?opt=val');
  assert.equal(withEquals.DATABASE_URL, 'postgres://user:pass@host:5432/db?opt=val');

  // Parsing a Buffer works too
  const fromBuffer = dotenv.parse(Buffer.from('BUFFERED=yes'));
  assert.equal(fromBuffer.BUFFERED, 'yes');

  console.log('dotenv-test:ok');
} catch (err) {
  console.error('dotenv-test:fail');
  console.error(err && err.stack ? err.stack : String(err));
  process.exit(1);
}
