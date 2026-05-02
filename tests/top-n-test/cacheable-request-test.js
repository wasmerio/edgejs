'use strict';

const assert = require('node:assert/strict');

(async () => {
  const imported = await import('cacheable-request');
  const CacheableRequest = imported.default || imported;

  // It should be a constructor/class
  assert.equal(typeof CacheableRequest, 'function', 'should export a function/class');

  // It should accept a request function (like http.request)
  const http = require('node:http');
  const cacheableRequest = new CacheableRequest(http.request);
  assert.ok(cacheableRequest, 'should create an instance with http.request');

  // The instance should itself be callable or have expected methods
  assert.equal(typeof cacheableRequest, 'function', 'instance should be callable');

  console.log('cacheable-request-test:ok');
})().catch((err) => {
  console.error('cacheable-request-test:fail');
  console.error(err && err.stack ? err.stack : String(err));
  process.exit(1);
});
