'use strict';

const assert = require('node:assert/strict');
const ci = require('ci-info');

try {
  // isCI should be a boolean
  assert.equal(typeof ci.isCI, 'boolean');

  // name should be a string or null (null when not on CI)
  assert.ok(ci.name === null || typeof ci.name === 'string',
    'name should be null or a string');

  // isPR should be a boolean or null
  assert.ok(ci.isPR === null || typeof ci.isPR === 'boolean',
    'isPR should be null or a boolean');

  // The module should have known CI vendor detection properties
  assert.equal(typeof ci.TRAVIS, 'boolean');
  assert.equal(typeof ci.CIRCLE, 'boolean');
  assert.equal(typeof ci.GITHUB_ACTIONS, 'boolean');
  assert.equal(typeof ci.GITLAB, 'boolean');
  assert.equal(typeof ci.JENKINS, 'boolean');
  assert.equal(typeof ci.BUILDKITE, 'boolean');

  // If we're not on CI, name should be null and vendor flags false
  if (!ci.isCI) {
    assert.equal(ci.name, null);
    assert.equal(ci.TRAVIS, false);
    assert.equal(ci.CIRCLE, false);
    assert.equal(ci.GITHUB_ACTIONS, false);
  }

  // If we are on CI, name should be a non-empty string
  if (ci.isCI) {
    assert.equal(typeof ci.name, 'string');
    assert.ok(ci.name.length > 0);
  }

  console.log('ci-info-test:ok');
} catch (err) {
  console.error('ci-info-test:fail');
  console.error(err && err.stack ? err.stack : String(err));
  process.exit(1);
}
