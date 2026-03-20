'use strict';

const assert = require('node:assert/strict');
const postcss = require('postcss');
const extractImports = require('postcss-modules-extract-imports');

(async () => {
  // The plugin should be usable as a postcss plugin
  const plugin = extractImports;
  assert.ok(plugin, 'plugin should be defined');

  // Process some CSS that uses composes ... from syntax (CSS Modules)
  const input = `.foo {
  composes: bar from "./other.css";
  color: red;
}`;

  const result = await postcss([plugin]).process(input, { from: undefined });

  // The result should be valid CSS output
  assert.equal(typeof result.css, 'string', 'should produce CSS output');
  assert.ok(result.css.length > 0, 'output should not be empty');

  // The plugin extracts the import, so "composes: bar from" should be transformed
  // The `:import("./other.css")` rule should appear or the composes should be simplified
  assert.ok(result.css.includes(':import'), 'should extract import into :import rule');

  // Process CSS without any imports - should pass through unchanged
  const plain = `.simple { color: blue; }`;
  const plainResult = await postcss([plugin]).process(plain, { from: undefined });
  assert.ok(plainResult.css.includes('color: blue'), 'plain CSS should pass through');

  console.log('postcss-modules-extract-imports-test:ok');
})().catch((err) => {
  console.error('postcss-modules-extract-imports-test:fail');
  console.error(err && err.stack ? err.stack : String(err));
  process.exit(1);
});
