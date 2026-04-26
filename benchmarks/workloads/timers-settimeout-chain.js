"use strict";

// Chain N sequential setTimeout(fn, 0) calls.
// Each callback fires, increments a counter, and schedules the next one.
// The chain runs to completion and exits.
//
// What it measures:
// - per-setTimeout dispatch cost in the event loop timer phase
// - callback scheduling and execution overhead across N iterations
//
// What it does not measure:
// - concurrent timer scheduling (timers are sequential here)
// - high-resolution timer behavior
// - timer cancellation or re-scheduling overhead

const N = 200;

let count = 0;
let checksum = 0;

function tick() {
  count += 1;
  checksum += count;
  if (count < N) {
    setTimeout(tick, 0);
  } else {
    // Deterministic: sum(1..200) = 20100
    console.log(String(checksum));
  }
}

setTimeout(tick, 0);
