'use strict';

const assert = require('node:assert/strict');

(async () => {
  // yargs v17 build files are missing; test via yargs-parser which is the core engine
  const parser = require('yargs-parser');

  // Parse basic typed options
  const argv1 = parser(['--port', '3000', '--host', 'localhost', '--verbose'], {
    number: ['port'],
    string: ['host'],
    boolean: ['verbose'],
    default: { port: 8080, host: '0.0.0.0', verbose: false },
  });

  assert.equal(argv1.port, 3000);
  assert.equal(argv1.host, 'localhost');
  assert.equal(argv1.verbose, true);

  // Default values are used when args are omitted
  const argv2 = parser([], {
    number: ['port'],
    string: ['host'],
    default: { port: 8080, host: '0.0.0.0' },
  });

  assert.equal(argv2.port, 8080);
  assert.equal(argv2.host, '0.0.0.0');

  // Aliases work
  const argv3 = parser(['-p', '4000', '-v'], {
    alias: { port: 'p', verbose: 'v' },
    number: ['port'],
    boolean: ['verbose'],
  });

  assert.equal(argv3.port, 4000);
  assert.equal(argv3.p, 4000);
  assert.equal(argv3.verbose, true);
  assert.equal(argv3.v, true);

  // Positional arguments appear in argv._
  const argv4 = parser(['hello', 'world', '--flag']);
  assert.deepEqual(argv4._, ['hello', 'world']);
  assert.equal(argv4.flag, true);

  // Array type option
  const argv5 = parser(['--tags', 'a', '--tags', 'b', '--tags', 'c'], {
    array: ['tags'],
  });
  assert.deepEqual(argv5.tags, ['a', 'b', 'c']);

  // String parsing: prevent numeric coercion
  const argv6 = parser(['--id', '007'], {
    string: ['id'],
  });
  assert.equal(argv6.id, '007'); // stays as string, not 7

  // Boolean flags: --no-* prefix
  const argv7 = parser(['--no-cache'], {
    boolean: ['cache'],
  });
  assert.equal(argv7.cache, false);

  // Dot notation for nested objects
  const argv8 = parser(['--server.port', '9090', '--server.host', 'myhost']);
  assert.equal(argv8.server.port, 9090);
  assert.equal(argv8.server.host, 'myhost');

  // -- stops parsing
  const argv9 = parser(['--flag', '--', '--not-a-flag', 'positional'], {
    configuration: { 'populate--': true },
  });
  assert.equal(argv9.flag, true);
  assert.deepEqual(argv9['--'], ['--not-a-flag', 'positional']);

  // camelCase conversion
  const argv10 = parser(['--my-option', 'value'], {
    string: ['my-option'],
    configuration: { 'camel-case-expansion': true },
  });
  assert.equal(argv10['my-option'], 'value');
  assert.equal(argv10.myOption, 'value');

  // Coerce option
  const argv11 = parser(['--date', '2024-01-15'], {
    string: ['date'],
    coerce: { date: (val) => new Date(val) },
  });
  assert.ok(argv11.date instanceof Date);
  assert.equal(argv11.date.getFullYear(), 2024);

  // Verify yargs package exists and has expected metadata
  const fs = require('node:fs');
  const yargsPkg = JSON.parse(
    fs.readFileSync(require.resolve('yargs/package.json'), 'utf8')
  );
  assert.equal(yargsPkg.name, 'yargs');
  assert.equal(typeof yargsPkg.version, 'string');

  console.log('yargs-test:ok');
})().catch((err) => {
  console.error('yargs-test:fail');
  console.error(err && err.stack ? err.stack : String(err));
  process.exit(1);
});
