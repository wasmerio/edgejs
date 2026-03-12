'use strict';

const events = require('events');
const ee = new events.EventEmitter();
ee.on('x', (a) => {
  process.stdout.write(`x=${String(a)}\n`);
});
ee.emit('x', 42);
process.stdout.write(`${typeof events.once} ${typeof events.on}\n`);
