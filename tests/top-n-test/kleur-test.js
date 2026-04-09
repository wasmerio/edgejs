'use strict';

const assert = require('node:assert/strict');

const kleur = require('kleur');

// Enable colors explicitly for testing
kleur.enabled = true;

// Basic colors
const red = kleur.red('error');
assert.equal(typeof red, 'string');
assert.ok(red.includes('error'));

const green = kleur.green('success');
assert.equal(typeof green, 'string');
assert.ok(green.includes('success'));

const blue = kleur.blue('info');
assert.equal(typeof blue, 'string');
assert.ok(blue.includes('info'));

// Bold and underline
const bold = kleur.bold('important');
assert.equal(typeof bold, 'string');
assert.ok(bold.includes('important'));

const underline = kleur.underline('link');
assert.equal(typeof underline, 'string');
assert.ok(underline.includes('link'));

// Chaining styles
const chained = kleur.bold().red('alert');
assert.equal(typeof chained, 'string');
assert.ok(chained.includes('alert'));

// Enabled flag controls whether ANSI codes are applied
// Disable colors and verify plain text output (no ANSI escape sequences)
kleur.enabled = false;
const plain = kleur.red('no-color');
assert.equal(plain, 'no-color');

// Re-enable colors and verify ANSI codes are present
kleur.enabled = true;
const colored = kleur.red('with-color');
assert.ok(colored.includes('with-color'));
assert.notEqual(colored, 'with-color', 'should include ANSI codes when enabled');

console.log('kleur-test:ok');
