'use strict';

const assert = require('node:assert/strict');
const browserslist = require('browserslist');

try {
  // Query 'last 2 versions' - should return an array of browser strings
  const last2 = browserslist('last 2 versions');
  assert.ok(Array.isArray(last2));
  assert.ok(last2.length > 0, 'last 2 versions should return results');
  // Each entry should be a string like "chrome 120"
  for (const entry of last2) {
    assert.equal(typeof entry, 'string');
    assert.ok(entry.length > 0);
  }

  // Query 'defaults'
  const defaults = browserslist('defaults');
  assert.ok(Array.isArray(defaults));
  assert.ok(defaults.length > 0, 'defaults should return results');
  for (const entry of defaults) {
    assert.equal(typeof entry, 'string');
  }

  // Query '>1%' - browsers with more than 1% usage
  const popular = browserslist('> 1%');
  assert.ok(Array.isArray(popular));
  assert.ok(popular.length > 0, '> 1% should return results');

  // Specific browser query
  const chromeOnly = browserslist('last 1 chrome version');
  assert.ok(Array.isArray(chromeOnly));
  assert.equal(chromeOnly.length, 1);
  assert.ok(chromeOnly[0].startsWith('chrome '));

  // Combined query with comma (union)
  const combined = browserslist('last 1 chrome version, last 1 firefox version');
  assert.ok(Array.isArray(combined));
  assert.ok(combined.length >= 2);

  // Coverage function - returns a usage percentage object
  const coverage = browserslist.coverage(defaults);
  assert.equal(typeof coverage, 'number');
  assert.ok(coverage > 0, 'coverage should be a positive number');

  console.log('browserslist-test:ok');
} catch (err) {
  console.error('browserslist-test:fail');
  console.error(err && err.stack ? err.stack : String(err));
  process.exit(1);
}
