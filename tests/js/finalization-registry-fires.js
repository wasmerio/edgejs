'use strict';

// Regression test for the V8-imports (WASIX) lane: FinalizationRegistry
// cleanup callbacks must actually fire.
//
// V8 posts the callback for a FinalizationRegistry with dead targets as a
// deferred foreground task (Heap::PostFinalizationRegistryCleanupTaskIfNeeded)
// after GC. In this lane the host's default enqueue path for that kind of
// task went nowhere -- the task was posted but nothing ever ran it -- so GC
// correctly reclaimed the dead targets, but FinalizationRegistry callbacks
// registered for them never fired. Node's own AbortSignal/WeakRef-based
// cleanup (lib/internal/abort_controller.js) depends on this, so the practical
// effect was a slow, unbounded per-request leak in every long-running edgejs
// process.
//
// Deliberately does NOT use v8.getHeapSnapshot() to force GC (as
// guest-finalizer-memory does): that path does not reliably drive this
// specific mechanism. Instead this churns real allocation pressure across
// setTimeout-spaced rounds, like a real request-serving workload would --
// which is what actually surfaced the bug originally.
//
// Self-contained (no node-test harness / common) so it runs directly through
// the WASIX runner, same as guest-finalizer-memory. A non-zero exit signals
// failure.

const assert = require('assert');

const ROUNDS = 30;
const PER_ROUND = 2000;
const ROUND_DELAY_MS = 100;
// The unpatched bridge finalizes exactly zero, ever, regardless of how much
// pressure or how many rounds; a fixed bridge finalizes the large majority of
// same-turn garbage within the first few rounds. Any real threshold above
// zero distinguishes a working drain from a broken one.
const MIN_FINALIZED_RATE = 0.5;

let finalized = 0;
let registered = 0;
const registry = new FinalizationRegistry(() => {
  finalized++;
});

function makeGarbage() {
  for (let i = 0; i < PER_ROUND; i++) {
    let obj = { data: new Array(500).fill(i), tag: `obj-${i}` };
    registry.register(obj, i);
    registered++;
    obj = null;
  }
  // Extra allocation pressure to encourage V8 to run GC cycles.
  let pressure = [];
  for (let i = 0; i < 500; i++) {
    pressure.push(new Array(2000).fill(Math.random()));
  }
  pressure = null;
}

function sleep(ms) {
  return new Promise((resolve) => setTimeout(resolve, ms));
}

async function main() {
  for (let round = 0; round < ROUNDS; round++) {
    makeGarbage();
    await sleep(ROUND_DELAY_MS);
  }

  const rate = registered === 0 ? 0 : finalized / registered;
  console.log(
    `finalization-registry-fires: finalized ${finalized}/${registered} ` +
    `(${(rate * 100).toFixed(1)}%)`);
  assert.ok(
    rate > MIN_FINALIZED_RATE,
    `FinalizationRegistry callbacks barely fired (${finalized}/${registered}, ` +
    `expected > ${(MIN_FINALIZED_RATE * 100).toFixed(0)}%) -- the deferred ` +
    'cleanup task V8 posts after GC is not being drained');
}

main().then(
  () => {},
  (err) => {
    console.error('finalization-registry-fires FAILED:', err && err.stack || err);
    process.exit(1);
  });
