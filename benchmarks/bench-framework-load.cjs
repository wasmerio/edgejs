// Fixed-request-count HTTP load with exact timing.
//
// autocannon's CLI only finishes an `--amount` run on its one-second tick, so
// its wall time is quantized to whole seconds. This wrapper drives the same
// client library, timestamps the final response itself, appends one JSON line
// and exits at once, so both its own elapsed time and the process wall time
// that hyperfine sees are exact.
//
// With a max-seconds argument the run also stops at that time limit, and its
// rate counts the responses received so far ("stopped_at_limit" is true).
//
// usage: node load.cjs <autocannon-dir> <url> <amount> <connections> <expected-body> <output.jsonl> [max-seconds]
'use strict';
const fs = require('node:fs');
const [autocannonDir, url, amountArg, connectionsArg, expectBody, output, maxSecondsArg] = process.argv.slice(2);
const autocannon = require(autocannonDir);
const amount = Number(amountArg);
const connections = Number(connectionsArg);

const latencies = new Float64Array(amount);
let responses = 0;
let non200 = 0;
let errors = 0;
let mismatches = 0;
let finished = false;

let stoppedAtLimit = false;

function finish(elapsedNs, complete) {
  if (finished) return;
  finished = true;
  const sorted = latencies.subarray(0, responses).sort();
  const at = (q) => (sorted.length ? sorted[Math.min(sorted.length - 1, Math.floor(q * sorted.length))] : null);
  const seconds = Number(elapsedNs) / 1e9;
  const ok = complete && non200 === 0 && errors === 0 && mismatches === 0;
  fs.appendFileSync(output, JSON.stringify({
    ok, requests: responses, seconds, req_per_sec: responses / seconds, stopped_at_limit: stoppedAtLimit,
    non200, errors, mismatches,
    latency_ms: { p50: at(0.5), p90: at(0.9), p99: at(0.99), max: at(1) },
  }) + '\n');
  process.exit(ok ? 0 : 1);
}

const start = process.hrtime.bigint();
const instance = autocannon({ url, connections, pipelining: 1, amount, timeout: 10, expectBody });
instance.on('response', (client, statusCode, bytes, responseTime) => {
  if (statusCode !== 200) non200++;
  latencies[responses++] = responseTime;
  if (responses === amount) finish(process.hrtime.bigint() - start, true);
});
instance.on('reqError', () => { errors++; });
instance.on('reqMismatch', () => { mismatches++; });
if (maxSecondsArg) {
  setTimeout(() => {
    stoppedAtLimit = true;
    finish(process.hrtime.bigint() - start, responses > 0);
  }, Number(maxSecondsArg) * 1000).unref();
}
// Only reached when errors or mismatches kept the run from completing.
instance.on('done', () => finish(process.hrtime.bigint() - start, false));
