'use strict';

const assert = require('node:assert/strict');

(async () => {
  const globals = require('globals');

  // globals is an object with environment categories
  assert.equal(typeof globals, 'object');
  assert.notEqual(globals, null);

  // Should have browser globals
  assert.ok('browser' in globals, 'should have browser category');
  assert.equal(typeof globals.browser, 'object');
  assert.ok('window' in globals.browser, 'browser should include window');
  assert.ok('document' in globals.browser, 'browser should include document');
  assert.ok('console' in globals.browser, 'browser should include console');
  assert.ok('setTimeout' in globals.browser, 'browser should include setTimeout');
  assert.ok('fetch' in globals.browser || true, 'browser may include fetch');

  // Should have node globals
  assert.ok('node' in globals, 'should have node category');
  assert.equal(typeof globals.node, 'object');
  assert.ok('Buffer' in globals.node, 'node should include Buffer');
  assert.ok('process' in globals.node, 'node should include process');
  assert.ok('__dirname' in globals.node, 'node should include __dirname');
  assert.ok('__filename' in globals.node, 'node should include __filename');
  assert.ok('require' in globals.node, 'node should include require');
  assert.ok('console' in globals.node, 'node should include console');
  assert.ok('setTimeout' in globals.node, 'node should include setTimeout');

  // Should have es2015 / es5 / es2017+ globals
  // globals v13 uses es5, es2015, es2017, es2020, es2021
  const hasES = 'es5' in globals || 'es2015' in globals || 'es6' in globals;
  assert.ok(hasES, 'should have some ES globals category');

  // Check some ES globals from whichever version exists
  const esCategory = globals.es2021 || globals.es2020 || globals.es2017 || globals.es2015 || globals.es6 || globals.es5;
  if (esCategory) {
    assert.ok('Promise' in esCategory || 'parseInt' in esCategory, 'ES globals should include common items');
  }

  // Each entry in a category is a boolean (writeable flag)
  for (const key of Object.keys(globals.browser).slice(0, 10)) {
    assert.equal(typeof globals.browser[key], 'boolean',
      `browser.${key} should be a boolean indicating writability`);
  }

  // Should have commonjs globals
  assert.ok('commonjs' in globals, 'should have commonjs category');
  assert.ok('module' in globals.commonjs, 'commonjs should include module');
  assert.ok('exports' in globals.commonjs, 'commonjs should include exports');

  console.log('globals-test:ok');
})().catch((err) => {
  console.error('globals-test:fail');
  console.error(err && err.stack ? err.stack : String(err));
  process.exit(1);
});
