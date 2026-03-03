'use strict';

const path = require('path');

const eventTarget = require(path.resolve(
  __dirname,
  '../../../../node-lib/internal/event_target.js'
));

if (typeof eventTarget.Event === 'function') {
  globalThis.Event = eventTarget.Event;
}
if (typeof eventTarget.EventTarget === 'function') {
  globalThis.EventTarget = eventTarget.EventTarget;
}
if (typeof eventTarget.CustomEvent === 'function') {
  globalThis.CustomEvent = eventTarget.CustomEvent;
}

module.exports = eventTarget;
