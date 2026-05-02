'use strict';

const assert = require('node:assert/strict');

(async () => {
  const colors = require('color-name');

  // Well-known CSS color names map to RGB arrays
  assert.deepEqual(colors.red, [255, 0, 0]);
  assert.deepEqual(colors.blue, [0, 0, 255]);
  assert.deepEqual(colors.white, [255, 255, 255]);
  assert.deepEqual(colors.black, [0, 0, 0]);
  assert.deepEqual(colors.green, [0, 128, 0]);
  assert.deepEqual(colors.lime, [0, 255, 0]);
  assert.deepEqual(colors.yellow, [255, 255, 0]);
  assert.deepEqual(colors.cyan, [0, 255, 255]);
  assert.deepEqual(colors.magenta, [255, 0, 255]);

  // CSS4 color: rebeccapurple (#663399)
  assert.deepEqual(colors.rebeccapurple, [102, 51, 153]);

  // Other notable colors
  assert.deepEqual(colors.coral, [255, 127, 80]);
  assert.deepEqual(colors.tomato, [255, 99, 71]);
  assert.deepEqual(colors.gold, [255, 215, 0]);
  assert.deepEqual(colors.navy, [0, 0, 128]);
  assert.deepEqual(colors.teal, [0, 128, 128]);

  // Every entry should be an array of 3 integers in [0, 255]
  const names = Object.keys(colors);
  assert.ok(names.length > 100, 'should have over 100 named colors');

  for (const name of names) {
    const rgb = colors[name];
    assert.ok(Array.isArray(rgb), `${name} should be an array`);
    assert.equal(rgb.length, 3, `${name} should have 3 channels`);
    for (let i = 0; i < 3; i++) {
      assert.ok(Number.isInteger(rgb[i]), `${name}[${i}] should be an integer`);
      assert.ok(rgb[i] >= 0 && rgb[i] <= 255, `${name}[${i}] should be in [0, 255]`);
    }
  }

  // Non-existent colors should be undefined
  assert.equal(colors.notarealcolor, undefined);
  assert.equal(colors.foobar, undefined);

  console.log('color-name-test:ok');
})().catch((err) => {
  console.error('color-name-test:fail');
  console.error(err && err.stack ? err.stack : String(err));
  process.exit(1);
});
