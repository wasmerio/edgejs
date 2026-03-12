'use strict';

const EventEmitter = require('events');
const assert = require('assert');

const EE = new EventEmitter();
const theErr = new Error('MyError');

EE.on(EventEmitter.errorMonitor, (e) => {
  process.stdout.write(`errorMonitor:${e.message}\n`);
});

process.stdout.write('phase1\n');
try {
  assert.throws(() => EE.emit('error', theErr), theErr);
  process.stdout.write('phase1-ok\n');
} catch (e) {
  process.stdout.write(`phase1-fail:${e && e.message}\n`);
}

process.stdout.write('phase2\n');
EE.once('error', (e) => process.stdout.write(`phase2-error:${e.message}\n`));
try {
  EE.emit('error', theErr);
  process.stdout.write('phase2-ok\n');
} catch (e) {
  process.stdout.write(`phase2-fail:${e && e.message}\n`);
}

process.stdout.write('phase3\n');
process.nextTick(() => {
  process.stdout.write('nextTick emit\n');
  EE.emit('error', theErr);
});
assert.rejects(EventEmitter.once(EE, 'notTriggered'), theErr).then(() => {
  process.stdout.write('phase3-resolved\n');
}, (e) => {
  process.stdout.write(`phase3-rejected:${e && e.message}\n`);
});
