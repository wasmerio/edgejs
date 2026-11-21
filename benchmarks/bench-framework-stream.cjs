// Holds steady HTTP load on a server and reports it in windows, for
// benchmarks too short to afford reconnecting and ramping up for each
// measurement.
//
// Prints {"ready":true} after the first response. Each "mark" line on stdin
// then prints one JSON line about the responses since the previous mark, while
// the load keeps running. "quit" exits.
//
// usage: node bench-framework-stream.cjs <autocannon-dir> <url> <connections> <expected-body>
'use strict';
const readline = require('node:readline');
const [autocannonDir, url, connectionsArg, expectBody] = process.argv.slice(2);
const autocannon = require(autocannonDir);

let latencies = [];
let non200 = 0;
let errors = 0;
let mismatches = 0;
let ready = false;
let windowStart = process.hrtime.bigint();

function mark() {
  const now = process.hrtime.bigint();
  const seconds = Number(now - windowStart) / 1e9;
  const sorted = Float64Array.from(latencies).sort();
  const at = (q) => (sorted.length ? sorted[Math.min(sorted.length - 1, Math.floor(q * sorted.length))] : null);
  const requests = sorted.length;
  console.log(JSON.stringify({
    ok: requests > 0 && non200 === 0 && errors === 0 && mismatches === 0,
    requests, seconds, req_per_sec: requests / seconds, non200, errors, mismatches,
    latency_ms: { p50: at(0.5), p90: at(0.9), p99: at(0.99), max: at(1) },
  }));
  latencies = [];
  non200 = 0;
  errors = 0;
  mismatches = 0;
  windowStart = process.hrtime.bigint();
}

const instance = autocannon({
  url, connections: Number(connectionsArg), pipelining: 1, duration: 3600, timeout: 10, expectBody,
});
instance.on('response', (client, statusCode, bytes, responseTime) => {
  if (statusCode !== 200) non200++;
  latencies.push(responseTime);
  if (!ready) {
    ready = true;
    console.log('{"ready":true}');
  }
});
instance.on('reqError', () => { errors++; });
instance.on('reqMismatch', () => { mismatches++; });

readline.createInterface({ input: process.stdin }).on('line', (line) => {
  if (line === 'mark') mark();
  if (line === 'quit') process.exit(0);
}).on('close', () => process.exit(0));
