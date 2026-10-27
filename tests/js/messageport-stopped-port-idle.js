'use strict';

// Regression test: a MessagePort that is not receiving (never started, or
// stopped) and has queued messages must leave the event loop idle.
//
// ProcessQueuedMessages used to re-arm the port's async handle whenever the
// queue was non-empty, so such a port woke the loop on every turn and burned a
// full core. Node only re-arms when the per-turn message budget runs out.
// Self-contained (no node-test harness); a non-zero exit signals failure.

const assert = require('assert');
const { MessageChannel } = require('worker_threads');

const WAIT_MS = 500;

const { port1, port2 } = new MessageChannel();
port2.postMessage('queued while port1 is not started');
port1.ref();

const before = process.cpuUsage();
const start = process.hrtime.bigint();
setTimeout(() => {
  const used = process.cpuUsage(before);
  const cpuMs = (used.user + used.system) / 1000;
  const wallMs = Number(process.hrtime.bigint() - start) / 1e6;
  port1.close();
  // A spinning port uses about one core for the whole wait.
  assert.ok(cpuMs < wallMs / 4,
            `port with queued messages kept the loop busy: ${cpuMs.toFixed(0)}ms CPU ` +
            `in ${wallMs.toFixed(0)}ms`);
  console.log(`messageport-stopped-port-idle: ${cpuMs.toFixed(0)}ms CPU in ${wallMs.toFixed(0)}ms`);
}, WAIT_MS);
