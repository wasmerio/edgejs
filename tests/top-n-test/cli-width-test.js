'use strict';

const assert = require('node:assert/strict');

(async () => {
  const mod = await import('cli-width');
  const cliWidth = mod.default || mod;

  // Call it and verify it returns a number
  const width = cliWidth();
  assert.equal(typeof width, 'number');

  // Should be a non-negative integer (0 if no TTY)
  assert.ok(width >= 0, 'width should be non-negative');
  assert.equal(width, Math.floor(width), 'width should be an integer');

  // When given a custom default, it should use it for non-TTY streams
  const customWidth = cliWidth({ defaultWidth: 120 });
  assert.equal(typeof customWidth, 'number');
  assert.ok(customWidth > 0, 'width with default should be positive');

  console.log('cli-width-test:ok');
})().catch((err) => {
  console.error('cli-width-test:fail');
  console.error(err && err.stack ? err.stack : String(err));
  process.exit(1);
});
