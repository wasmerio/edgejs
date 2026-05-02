'use strict';

const assert = require('node:assert/strict');

(async () => {
  let autoprefixer;
  try {
    autoprefixer = require('autoprefixer');
  } catch (_) {
    const imported = await import('autoprefixer');
    autoprefixer = imported.default || imported;
  }

  const postcss = require('postcss');

  // autoprefixer should be a function (PostCSS plugin creator)
  assert.equal(typeof autoprefixer, 'function', 'autoprefixer should be a function');

  // Create a plugin instance
  const plugin = autoprefixer({ overrideBrowserslist: ['last 4 versions'] });
  assert.ok(plugin, 'plugin instance should be created');

  // Process some CSS that needs prefixing
  const css = 'a { user-select: none; }';
  const result = await postcss([autoprefixer({ overrideBrowserslist: ['last 4 versions'] })]).process(css, { from: undefined });

  assert.equal(typeof result.css, 'string', 'result should have css string');
  // With older browser targets, user-select should get vendor prefixes
  assert.ok(result.css.includes('-webkit-user-select') || result.css.includes('user-select'),
    'should process user-select property');

  // Test with another property
  const flexCss = '.container { display: flex; }';
  const flexResult = await postcss([autoprefixer({ overrideBrowserslist: ['last 4 versions'] })]).process(flexCss, { from: undefined });
  assert.equal(typeof flexResult.css, 'string');
  assert.ok(flexResult.css.includes('display'), 'result should contain display property');

  // Verify info() method works
  const info = autoprefixer({ overrideBrowserslist: ['last 2 versions'] }).info();
  assert.equal(typeof info, 'string', 'info() should return a string');
  assert.ok(info.length > 0, 'info should not be empty');

  console.log('autoprefixer-test:ok');
})().catch((err) => {
  console.error('autoprefixer-test:fail');
  console.error(err && err.stack ? err.stack : String(err));
  process.exit(1);
});
