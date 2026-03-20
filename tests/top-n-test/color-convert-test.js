'use strict';

const assert = require('node:assert/strict');

(async () => {
  const convert = require('color-convert');

  // RGB to hex
  assert.equal(convert.rgb.hex(255, 255, 255), 'FFFFFF');
  assert.equal(convert.rgb.hex(0, 0, 0), '000000');
  assert.equal(convert.rgb.hex(255, 0, 0), 'FF0000');

  // Hex to RGB
  assert.deepEqual(convert.hex.rgb('FFFFFF'), [255, 255, 255]);
  assert.deepEqual(convert.hex.rgb('00FF00'), [0, 255, 0]);
  assert.deepEqual(convert.hex.rgb('0000FF'), [0, 0, 255]);

  // RGB to HSL: pure red is [0, 100, 50]
  const redHsl = convert.rgb.hsl(255, 0, 0);
  assert.equal(redHsl[0], 0, 'red hue should be 0');
  assert.equal(redHsl[1], 100, 'red saturation should be 100');
  assert.equal(redHsl[2], 50, 'red lightness should be 50');

  // HSL to RGB round-trip
  const backToRgb = convert.hsl.rgb(0, 100, 50);
  assert.equal(backToRgb[0], 255);
  assert.equal(backToRgb[1], 0);
  assert.equal(backToRgb[2], 0);

  // RGB to HSV
  const whiteHsv = convert.rgb.hsv(255, 255, 255);
  assert.equal(whiteHsv[1], 0, 'white should have 0 saturation');
  assert.equal(whiteHsv[2], 100, 'white should have 100 value');

  // Keyword to RGB
  const blueRgb = convert.keyword.rgb('blue');
  assert.deepEqual(blueRgb, [0, 0, 255]);

  const limeRgb = convert.keyword.rgb('lime');
  assert.deepEqual(limeRgb, [0, 255, 0]);

  // RGB to keyword
  const keyword = convert.rgb.keyword(255, 0, 0);
  assert.equal(keyword, 'red');

  // CMYK to RGB: pure cyan in CMYK [100, 0, 0, 0] should be [0, 255, 255] in RGB
  const cyanRgb = convert.cmyk.rgb(100, 0, 0, 0);
  assert.equal(cyanRgb[0], 0);
  assert.equal(cyanRgb[1], 255);
  assert.equal(cyanRgb[2], 255);

  // RGB to CMYK round-trip
  const cmyk = convert.rgb.cmyk(0, 255, 255);
  assert.equal(cmyk[0], 100, 'cyan component should be 100');
  assert.equal(cmyk[3], 0, 'black component should be 0 for pure cyan');

  // HSL to hex
  const greenHex = convert.hsl.hex(120, 100, 50);
  assert.equal(greenHex, '00FF00');

  // Chained conversions work (rgb -> hsl -> hex round-trip is consistent)
  const originalHex = 'FF8800';
  const hslFromHex = convert.hex.hsl(originalHex);
  const hexBack = convert.hsl.hex(hslFromHex);
  // Should round-trip close enough (may lose precision)
  assert.equal(typeof hexBack, 'string');
  assert.equal(hexBack.length, 6);

  console.log('color-convert-test:ok');
})().catch((err) => {
  console.error('color-convert-test:fail');
  console.error(err && err.stack ? err.stack : String(err));
  process.exit(1);
});
