'use strict';

const assert = require('node:assert/strict');

(async () => {
  const mod = require('minimatch');
  const minimatch = typeof mod === 'function' ? mod : (mod.minimatch || mod.default);
  const Minimatch = mod.Minimatch || (mod.default && mod.default.Minimatch);

  // Basic glob matching
  assert.equal(minimatch('app.js', '*.js'), true);
  assert.equal(minimatch('app.ts', '*.js'), false);
  assert.equal(minimatch('README.md', '*.md'), true);
  assert.equal(minimatch('src/index.js', '*.js'), false, 'single star should not cross directory boundaries');

  // Globstar for deep matching
  assert.equal(minimatch('src/index.ts', '**/*.ts'), true);
  assert.equal(minimatch('src/components/Button.ts', '**/*.ts'), true);
  assert.equal(minimatch('index.ts', '**/*.ts'), true);
  assert.equal(minimatch('src/index.js', '**/*.ts'), false);

  // Negation pattern
  assert.equal(minimatch('types.d.ts', '*.d.ts'), true);
  assert.equal(minimatch('index.ts', '*.d.ts'), false);

  // Options: dot files
  assert.equal(minimatch('.gitignore', '*', { dot: false }), false, 'dot files should not match without dot option');
  assert.equal(minimatch('.gitignore', '*', { dot: true }), true, 'dot files should match with dot option');

  // Options: nocase for case-insensitive matching
  assert.equal(minimatch('README.MD', '*.md', { nocase: true }), true);
  assert.equal(minimatch('README.MD', '*.md', { nocase: false }), false);

  // Brace expansion
  assert.equal(minimatch('file.js', '*.{js,ts}'), true);
  assert.equal(minimatch('file.ts', '*.{js,ts}'), true);
  assert.equal(minimatch('file.css', '*.{js,ts}'), false);

  // Character class
  assert.equal(minimatch('a1.txt', 'a[0-9].txt'), true);
  assert.equal(minimatch('ab.txt', 'a[0-9].txt'), false);

  // Minimatch class for reusable patterns
  if (Minimatch) {
    const matcher = new Minimatch('**/*.js');
    assert.equal(matcher.match('src/index.js'), true);
    assert.equal(matcher.match('lib/utils/helpers.js'), true);
    assert.equal(matcher.match('style.css'), false);

    // The set property contains the parsed pattern
    assert.ok(Array.isArray(matcher.set), 'Minimatch instance should have a set array');
  }

  // filter() returns a function suitable for Array.prototype.filter
  if (typeof minimatch.filter === 'function') {
    const files = ['app.js', 'app.css', 'index.js', 'style.css'];
    const jsFiles = files.filter(minimatch.filter('*.js'));
    assert.deepEqual(jsFiles, ['app.js', 'index.js']);
  }

  // match() filters an array of paths
  if (typeof minimatch.match === 'function') {
    const paths = ['src/a.ts', 'src/b.js', 'lib/c.ts'];
    const matched = minimatch.match(paths, '**/*.ts');
    assert.ok(matched.includes('src/a.ts'));
    assert.ok(matched.includes('lib/c.ts'));
    assert.ok(!matched.includes('src/b.js'));
  }

  console.log('minimatch-test:ok');
})().catch((err) => {
  console.error('minimatch-test:fail');
  console.error(err && err.stack ? err.stack : String(err));
  process.exit(1);
});
