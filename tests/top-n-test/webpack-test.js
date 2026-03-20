'use strict';

const assert = require('node:assert/strict');
const path = require('node:path');
const os = require('node:os');
const fs = require('node:fs');

(async () => {
  const webpack = require('webpack');

  // Verify webpack is a callable function
  assert.equal(typeof webpack, 'function', 'webpack should be a function');

  // Check version string
  assert.equal(typeof webpack.version, 'string', 'should expose a version string');
  assert.ok(webpack.version.length > 0);

  // Create a compiler with a simple config (don't run a build)
  const tmpDir = fs.mkdtempSync(path.join(os.tmpdir(), 'webpack-smoke-'));
  const entryFile = path.join(tmpDir, 'index.js');
  fs.writeFileSync(entryFile, 'module.exports = 42;');

  const compiler = webpack({
    mode: 'production',
    entry: entryFile,
    output: {
      path: path.join(tmpDir, 'dist'),
      filename: 'bundle.js',
    },
  });

  // Verify the compiler object has the expected shape
  assert.equal(typeof compiler, 'object', 'compiler should be an object');
  assert.equal(typeof compiler.run, 'function', 'compiler should have a run method');
  assert.equal(typeof compiler.watch, 'function', 'compiler should have a watch method');
  assert.equal(typeof compiler.close, 'function', 'compiler should have a close method');

  // Verify hooks are accessible
  assert.ok(compiler.hooks, 'compiler should have hooks');
  assert.ok(compiler.hooks.compile, 'should have compile hook');
  assert.ok(compiler.hooks.done, 'should have done hook');
  assert.ok(compiler.hooks.emit, 'should have emit hook');

  // Verify some important webpack exports
  assert.ok(webpack.DefinePlugin, 'should export DefinePlugin');
  assert.ok(webpack.HotModuleReplacementPlugin, 'should export HotModuleReplacementPlugin');

  // Cleanup
  fs.rmSync(tmpDir, { recursive: true, force: true });

  console.log('webpack-test:ok');
})().catch((err) => {
  console.error('webpack-test:fail');
  console.error(err && err.stack ? err.stack : String(err));
  process.exit(1);
});
