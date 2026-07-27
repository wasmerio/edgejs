'use strict';

// Regression test for guest N-API finalizer dispatch (ECO-415), V8-imports
// (WASIX) lane.
//
// `Buffer.allocUnsafe(n)` for n larger than the Buffer pool routes through
// `createUnsafeArrayBuffer`, which does a guest-side `std::malloc` and hands V8
// an *external* ArrayBuffer whose finalizer (`ExternalArrayBufferFinalize` ->
// `free`) runs in the guest. That finalize callback is a wasm function pointer
// the host cannot invoke directly; the bridge must re-enter the guest on its
// deferred finalizer drain to run it.
//
// If that dispatch does not happen, every large `allocUnsafe` leaks its guest
// allocation. The guest lives in a wasm32 linear memory capped at 4 GiB (and
// `memory.grow` never shrinks), so an unbounded leak makes the heap climb until
// `malloc` fails and `allocUnsafe` throws. This test churns well over 4 GiB in
// aggregate while dropping each buffer and forcing collection, so it can only
// run to completion if the guest finalizer actually reclaims memory.
//
// Self-contained (no node-test harness / common) so it runs directly via the
// edgejs-owned tests/js lane. A non-zero exit signals failure.
//
// Notes on this lane: `global.gc()` is not wired to V8 here and
// `process.memoryUsage().rss` reports 0, so neither is usable. A heap snapshot
// forces a full GC; successful completion past the 4 GiB ceiling is the signal.

const assert = require('assert');
const v8 = require('v8');

const CHUNK = 4 * 1024 * 1024;   // 4 MiB: safely bypasses the Buffer pool.
const ITERS = 1536;              // 6 GiB aggregate, ~1.5x the 4 GiB guest cap.
const FORCE_EVERY = 64;          // How often to force GC + drain finalizers.

function forceGcAndDrain() {
  // A heap snapshot forces a full GC, collecting the dropped ArrayBuffers.
  try {
    v8.getHeapSnapshot().pause().read();
  } catch {
    if (typeof global.gc === 'function') global.gc();
  }
  // A turn of the event loop runs the bridge's deferred finalizer drain, which
  // is where the guest finalize callbacks are dispatched.
  return new Promise((resolve) => setImmediate(resolve));
}

async function main() {
  let completed = 0;
  for (let i = 0; i < ITERS; i++) {
    let b = Buffer.allocUnsafe(CHUNK);
    // Touch both ends so the pages are actually committed.
    b[0] = i & 0xff;
    b[CHUNK - 1] = 0xab;
    assert.strictEqual(b[CHUNK - 1], 0xab);
    b = null;
    completed++;

    if (i % FORCE_EVERY === 0) {
      await forceGcAndDrain();
    }
  }
  await forceGcAndDrain();

  assert.strictEqual(completed, ITERS);
  console.log(
    `guest-finalizer-memory: reclaimed across ${ITERS} iters ` +
    `(${((ITERS * CHUNK) / (1024 ** 3)).toFixed(1)} GiB aggregate, ` +
    `${(CHUNK / (1024 ** 2)).toFixed(0)} MiB each) without exhausting the ` +
    'guest heap');
}

main().then(
  () => {},
  (err) => {
    console.error('guest-finalizer-memory FAILED:', err && err.stack || err);
    process.exit(1);
  });
