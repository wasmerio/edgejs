// Touches a representative spread of node: builtins so a single run populates
// the consolidated builtins bytecode cache (<binary>.builtins.<engine-tag>)
// broadly, not just the bootstrap set every process already loads. Run via
// `make precompile-builtins`; the produced file is shipped next to the binary.
//
// The builtins cache is keyed by engine tag + per-builtin source hash, so this
// driver never needs updating when builtins change — only the set it touches
// bounds how much of the cache is pre-populated.
const builtins = [
  'assert', 'async_hooks', 'buffer', 'child_process', 'console', 'crypto',
  'dgram', 'diagnostics_channel', 'dns', 'events', 'fs', 'fs/promises',
  'http', 'http2', 'https', 'net', 'os', 'path', 'perf_hooks', 'process',
  'punycode', 'querystring', 'readline', 'stream', 'stream/promises',
  'string_decoder', 'timers', 'timers/promises', 'tls', 'tty', 'url', 'util',
  'v8', 'vm', 'worker_threads', 'zlib',
];

let loaded = 0;
for (const name of builtins) {
  try {
    await import(`node:${name}`);
    loaded++;
  } catch {
    // A builtin missing on one engine/build is fine; keep going.
  }
}

console.log(`precompile-builtins: touched ${loaded}/${builtins.length} builtins`);
