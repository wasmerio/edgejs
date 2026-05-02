'use strict';

const assert = require('node:assert/strict');

(async () => {
  const mod = await import('chalk');
  const chalk = mod.default;

  // Basic color methods return strings
  const red = chalk.red('error');
  assert.equal(typeof red, 'string');
  assert.ok(red.includes('error'), 'red styled text should contain the original string');

  const green = chalk.green('success');
  assert.ok(green.includes('success'));

  const blue = chalk.blue('info');
  assert.ok(blue.includes('info'));

  // Chaining styles: bold + underline
  const boldUnderline = chalk.bold.underline('important');
  assert.equal(typeof boldUnderline, 'string');
  assert.ok(boldUnderline.includes('important'));

  // Nested styles
  const nested = chalk.red('red ' + chalk.bold('bold-red') + ' red-again');
  assert.ok(nested.includes('red '));
  assert.ok(nested.includes('bold-red'));
  assert.ok(nested.includes('red-again'));

  // Multiple arguments get space-joined
  const multi = chalk.yellow('one', 'two', 'three');
  assert.ok(multi.includes('one'));
  assert.ok(multi.includes('two'));
  assert.ok(multi.includes('three'));

  // RGB and hex colors
  const rgbText = chalk.rgb(255, 136, 0)('orange');
  assert.equal(typeof rgbText, 'string');
  assert.ok(rgbText.includes('orange'));

  const hexText = chalk.hex('#FF8800')('hex-orange');
  assert.equal(typeof hexText, 'string');
  assert.ok(hexText.includes('hex-orange'));

  // Background colors
  const bgRed = chalk.bgRed('warning');
  assert.ok(bgRed.includes('warning'));

  // The level property controls color support (0 = none, 1-3 = increasing)
  assert.equal(typeof chalk.level, 'number');
  assert.ok(chalk.level >= 0 && chalk.level <= 3);

  // Chalk instance via Chalk constructor
  const { Chalk } = mod;
  if (Chalk) {
    const custom = new Chalk({ level: 1 });
    const styled = custom.red('custom');
    assert.equal(typeof styled, 'string');
    assert.ok(styled.includes('custom'));
  }

  console.log('chalk-test:ok');
})().catch((err) => {
  console.error('chalk-test:fail');
  console.error(err && err.stack ? err.stack : String(err));
  process.exit(1);
});
