'use strict';

// Astro's streamed dashboard rendering made 8,888 encodeInto calls per request,
// mostly for short strings. Reuse the encoder and destination to isolate the
// binding cost from allocation, HTTP transport, and process startup.
const encoder = new TextEncoder();
const destination = new Uint8Array(256);
const inputs = [
  'a',
  '</span>',
  'astro-slot',
  '<div class="sidebar-link">',
  '\u00e9\ud83d\ude00',
];
const callsPerBatch = 8888;
const batchCount = 8;

for (let i = 0; i < 10000; i++) {
  encoder.encodeInto(inputs[i % inputs.length], destination);
}

const batches = [];
for (let batch = 0; batch < batchCount; batch++) {
  const start = performance.now();
  let bytes = 0;
  for (let i = 0; i < callsPerBatch; i++) {
    bytes += encoder.encodeInto(inputs[i % inputs.length], destination).written;
  }
  batches.push({ milliseconds: performance.now() - start, bytes });
}

console.log(JSON.stringify({ callsPerBatch, batchCount, batches }));
