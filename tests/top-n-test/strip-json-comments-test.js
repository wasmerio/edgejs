'use strict';

const assert = require('node:assert/strict');

(async () => {
  const mod = await import('strip-json-comments');
  const stripJsonComments = mod.default;

  // Strip single-line comments (//)
  const withSingle = '{\n  "name": "test" // this is a name\n}';
  const stripped1 = stripJsonComments(withSingle);
  assert.ok(!stripped1.includes('//'), 'single-line comment should be removed');
  const parsed1 = JSON.parse(stripped1);
  assert.equal(parsed1.name, 'test');

  // Strip block comments (/* */)
  const withBlock = '{\n  /* version number */\n  "version": "1.0.0"\n}';
  const stripped2 = stripJsonComments(withBlock);
  assert.ok(!stripped2.includes('/*'), 'block comment start should be removed');
  assert.ok(!stripped2.includes('*/'), 'block comment end should be removed');
  const parsed2 = JSON.parse(stripped2);
  assert.equal(parsed2.version, '1.0.0');

  // Preserve strings that contain // inside them
  const withUrlInString = '{\n  "url": "https://example.com"\n}';
  const stripped3 = stripJsonComments(withUrlInString);
  const parsed3 = JSON.parse(stripped3);
  assert.equal(parsed3.url, 'https://example.com', 'URL with // inside string should be preserved');

  // Combined: both comment types and strings with comment-like content
  const combined = [
    '{',
    '  // top-level comment',
    '  "homepage": "https://github.com/foo", /* inline block */',
    '  "count": 42',
    '}',
  ].join('\n');
  const stripped4 = stripJsonComments(combined);
  const parsed4 = JSON.parse(stripped4);
  assert.equal(parsed4.homepage, 'https://github.com/foo');
  assert.equal(parsed4.count, 42);

  console.log('strip-json-comments-test:ok');
})().catch((err) => {
  console.error('strip-json-comments-test:fail');
  console.error(err && err.stack ? err.stack : String(err));
  process.exit(1);
});
