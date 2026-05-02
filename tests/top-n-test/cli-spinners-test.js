'use strict';

const assert = require('node:assert/strict');

const spinners = require('cli-spinners');

// Access the dots spinner
const dots = spinners.dots;
assert.ok(dots, 'dots spinner should exist');
assert.ok(Array.isArray(dots.frames), 'dots should have a frames array');
assert.ok(dots.frames.length > 0, 'dots frames should not be empty');
assert.equal(typeof dots.interval, 'number', 'dots should have a numeric interval');
assert.ok(dots.interval > 0, 'interval should be positive');

// Each frame should be a string
for (const frame of dots.frames) {
  assert.equal(typeof frame, 'string');
}

// Access other spinners
const line = spinners.line;
assert.ok(line, 'line spinner should exist');
assert.ok(Array.isArray(line.frames));
assert.equal(typeof line.interval, 'number');

const bouncingBar = spinners.bouncingBar;
assert.ok(bouncingBar, 'bouncingBar spinner should exist');
assert.ok(Array.isArray(bouncingBar.frames));
assert.equal(typeof bouncingBar.interval, 'number');

// The module should have many spinners
const spinnerNames = Object.keys(spinners);
assert.ok(spinnerNames.length > 10, 'should have many spinner definitions');

// Every spinner should have frames and interval
for (const name of spinnerNames) {
  const spinner = spinners[name];
  assert.ok(Array.isArray(spinner.frames), `${name} should have frames`);
  assert.equal(typeof spinner.interval, 'number', `${name} should have interval`);
}

console.log('cli-spinners-test:ok');
