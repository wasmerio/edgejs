'use strict';

const assert = require('node:assert/strict');

(async () => {
  const prettier = require('prettier');

  // Format basic JavaScript with default options
  const formatted = await prettier.format('const   x=1;const y  =  2;', { parser: 'babel' });
  assert.ok(formatted.includes('const x = 1'));
  assert.ok(formatted.includes('const y = 2'));

  // Format with semicolons disabled
  const noSemi = await prettier.format('const a = 1;', { parser: 'babel', semi: false });
  assert.ok(!noSemi.trim().endsWith(';'), 'should not have trailing semicolon');

  // Format with single quotes
  const singleQuote = await prettier.format('const s = "hello";', { parser: 'babel', singleQuote: true });
  assert.ok(singleQuote.includes("'hello'"), 'should use single quotes');

  // Format with double quotes (default)
  const doubleQuote = await prettier.format("const s = 'hello';", { parser: 'babel', singleQuote: false });
  assert.ok(doubleQuote.includes('"hello"'), 'should use double quotes');

  // Format JSON
  const uglyJson = '{"name":"test","version":"1.0.0","deps":{"a":"1","b":"2"}}';
  const prettyJson = await prettier.format(uglyJson, { parser: 'json' });
  assert.ok(prettyJson.includes('\n'), 'formatted JSON should be multi-line');
  // Verify it's still valid JSON
  const parsed = JSON.parse(prettyJson);
  assert.equal(parsed.name, 'test');
  assert.equal(parsed.version, '1.0.0');

  // Format with specific printWidth
  const narrow = await prettier.format(
    'const result = someFunction(argument1, argument2, argument3, argument4);',
    { parser: 'babel', printWidth: 40 }
  );
  // With narrow printWidth, long calls should be broken across lines
  const lines = narrow.trim().split('\n');
  assert.ok(lines.length > 1, 'narrow printWidth should cause line breaks');

  // Format with tab indentation
  const tabbed = await prettier.format('function foo() {\nreturn 1;\n}', { parser: 'babel', useTabs: true });
  assert.ok(tabbed.includes('\t'), 'should contain tab characters');

  // check() returns false for unformatted code
  const isFormatted = await prettier.check('const   x=1', { parser: 'babel' });
  assert.equal(isFormatted, false);

  // check() returns true for already-formatted code
  const alreadyFormatted = await prettier.check('const x = 1;\n', { parser: 'babel' });
  assert.equal(alreadyFormatted, true);

  console.log('prettier-test:ok');
})().catch((err) => {
  console.error('prettier-test:fail');
  console.error(err && err.stack ? err.stack : String(err));
  process.exit(1);
});
