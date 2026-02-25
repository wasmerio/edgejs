'use strict';

// Minimal shim for require('module'). _initPaths() is called by tests like
// test-require-dot.js; when NODE_PATH is used we rely on the loader reading
// process.env.NODE_PATH (via setenv sync) and getenv in the loader.
function _initPaths() {
  // No-op: loader reads NODE_PATH from environment when present.
}

module.exports = {
  _initPaths,
  builtinModules: [
    'assert',
    'buffer',
    'console',
    'domain',
    'events',
    'fs',
    'module',
    'path',
    'querystring',
    'stream',
    'stream/promises',
    'stream/web',
    'string_decoder',
    'url',
    'util',
    'worker_threads',
  ],
};
