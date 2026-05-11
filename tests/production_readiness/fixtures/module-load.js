'use strict';

const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const zlib = require('node:zlib');

const payload = Buffer.from('production-readiness');
const compressed = zlib.gzipSync(payload);
const roundtrip = zlib.gunzipSync(compressed).toString('utf8');

if (roundtrip !== 'production-readiness') {
  throw new Error(`zlib roundtrip mismatch: ${roundtrip}`);
}

const root = path.parse(process.cwd()).root;
if (!root || typeof os.platform() !== 'string') {
  throw new Error('basic platform/module checks failed');
}

if (typeof fs.existsSync !== 'function') {
  throw new Error('node:fs did not load correctly');
}

console.log('production-readiness:module-load');
