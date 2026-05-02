'use strict';

const assert = require('node:assert/strict');

(async () => {
  const debug = require('debug');

  // Create namespaced loggers
  const appLog = debug('myapp:server');
  const dbLog = debug('myapp:db');
  const authLog = debug('myapp:auth');

  assert.equal(typeof appLog, 'function');
  assert.equal(typeof dbLog, 'function');

  // Enable specific namespaces with wildcards
  debug.enable('myapp:*');
  assert.equal(debug.enabled('myapp:server'), true);
  assert.equal(debug.enabled('myapp:db'), true);
  assert.equal(debug.enabled('myapp:auth'), true);
  assert.equal(debug.enabled('other:thing'), false);

  // The logger instance should reflect enabled state
  assert.equal(appLog.enabled, true);

  // Capture output by overriding the log function
  let captured = '';
  appLog.log = function (...args) {
    captured = args.join(' ');
  };
  appLog('listening on port %d', 3000);
  assert.ok(captured.includes('3000'), 'formatted output should contain the port number');

  // Enable only a subset using comma-separated namespaces
  debug.enable('myapp:db,myapp:auth');
  assert.equal(debug.enabled('myapp:server'), false);
  assert.equal(debug.enabled('myapp:db'), true);
  assert.equal(debug.enabled('myapp:auth'), true);

  // Disable all namespaces
  debug.disable();
  assert.equal(debug.enabled('myapp:server'), false);
  assert.equal(debug.enabled('myapp:db'), false);
  assert.equal(debug.enabled('myapp:auth'), false);

  // Exclusion patterns: enable all except db
  debug.enable('myapp:*,-myapp:db');
  assert.equal(debug.enabled('myapp:server'), true);
  assert.equal(debug.enabled('myapp:db'), false);

  // Each logger has a namespace property
  assert.equal(appLog.namespace, 'myapp:server');
  assert.equal(dbLog.namespace, 'myapp:db');

  // The extend method creates child namespaces
  const childLog = appLog.extend('request');
  assert.equal(childLog.namespace, 'myapp:server:request');

  // Clean up
  debug.disable();

  console.log('debug-test:ok');
})().catch((err) => {
  console.error('debug-test:fail');
  console.error(err && err.stack ? err.stack : String(err));
  process.exit(1);
});
