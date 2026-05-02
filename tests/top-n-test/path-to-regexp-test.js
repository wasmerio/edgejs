'use strict';

const assert = require('node:assert/strict');
const { pathToRegexp, match, compile } = require('path-to-regexp');

try {
  // Match '/user/:id' and extract params
  const keys = [];
  const re = pathToRegexp('/user/:id', keys);
  assert.ok(re instanceof RegExp, 'should return a RegExp');
  assert.equal(keys.length, 1);
  assert.equal(keys[0].name, 'id');

  const m1 = re.exec('/user/42');
  assert.ok(m1, 'should match /user/42');
  assert.equal(m1[1], '42');

  assert.equal(re.exec('/user/'), null, 'should not match empty id');

  // Use the match helper for cleaner param extraction
  const matchUser = match('/user/:id', { decode: decodeURIComponent });
  const result = matchUser('/user/123');
  assert.ok(result, 'match should succeed');
  assert.equal(result.params.id, '123');

  assert.equal(matchUser('/other/123'), false, 'should not match different path');

  // Compile a path pattern into a function that generates paths
  const toPath = compile('/user/:id');
  assert.equal(toPath({ id: 99 }), '/user/99');
  assert.equal(toPath({ id: 'alice' }), '/user/alice');

  // Optional parameters
  const keysOpt = [];
  const reOpt = pathToRegexp('/files/:path*', keysOpt);
  assert.ok(reOpt.exec('/files/'), 'should match with no wildcard segments');
  assert.ok(reOpt.exec('/files/a/b/c'), 'should match with multiple segments');

  console.log('path-to-regexp-test:ok');
} catch (err) {
  console.error('path-to-regexp-test:fail');
  console.error(err && err.stack ? err.stack : String(err));
  process.exit(1);
}
