'use strict';

const assert = require('node:assert/strict');

(async () => {
  const pw = require('playwright-core');

  // Browser types should exist
  assert.equal(typeof pw.chromium, 'object', 'chromium browser type should exist');
  assert.equal(typeof pw.firefox, 'object', 'firefox browser type should exist');
  assert.equal(typeof pw.webkit, 'object', 'webkit browser type should exist');

  // Each browser type should have a launch method
  assert.equal(typeof pw.chromium.launch, 'function', 'chromium should have launch');
  assert.equal(typeof pw.firefox.launch, 'function', 'firefox should have launch');
  assert.equal(typeof pw.webkit.launch, 'function', 'webkit should have launch');

  // Each browser type should have a name
  assert.equal(pw.chromium.name(), 'chromium');
  assert.equal(pw.firefox.name(), 'firefox');
  assert.equal(pw.webkit.name(), 'webkit');

  // Devices list should be populated
  assert.equal(typeof pw.devices, 'object', 'devices should be an object');
  const deviceNames = Object.keys(pw.devices);
  assert.ok(deviceNames.length > 0, 'should have known devices');
  // Check a well-known device exists
  assert.ok(pw.devices['iPhone 13'] || pw.devices['Pixel 5'] || deviceNames.length > 10,
    'should contain common device definitions');

  // A device entry should have viewport and userAgent
  const someDevice = pw.devices[deviceNames[0]];
  assert.ok(someDevice.viewport, 'device should have viewport');
  assert.equal(typeof someDevice.userAgent, 'string', 'device should have userAgent string');

  // Selectors should exist
  assert.equal(typeof pw.selectors, 'object', 'selectors should exist');

  // Errors should be exported
  assert.equal(typeof pw.errors, 'object', 'errors should exist');
  assert.equal(typeof pw.errors.TimeoutError, 'function', 'TimeoutError should be a constructor');

  console.log('playwright-core-test:ok');
})().catch((err) => {
  console.error('playwright-core-test:fail');
  console.error(err && err.stack ? err.stack : String(err));
  process.exit(1);
});
