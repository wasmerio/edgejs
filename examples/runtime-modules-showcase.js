'use strict';

const assert = require('assert');
const fs = require('fs');
const os = require('os');
const path = require('path');
const { pathToFileURL } = require('url');
const Stream = require('stream');
const util = require('util');
const Module = require('module');
const net = require('net');
const BufferCtor = globalThis.Buffer;

function concatBytes(chunks) {
  let total = 0;
  for (const chunk of chunks) total += chunk.length;
  const out = new Uint8Array(total);
  let offset = 0;
  for (const chunk of chunks) {
    out.set(chunk, offset);
    offset += chunk.length;
  }
  return out;
}

function toHex(bytes) {
  const parts = [];
  for (let i = 0; i < bytes.length; i += 1) {
    parts.push(bytes[i].toString(16).padStart(2, '0'));
  }
  return parts.join('');
}

function toAsciiPreview(bytes, maxLen) {
  let s = '';
  const len = Math.min(bytes.length, maxLen);
  for (let i = 0; i < len; i += 1) {
    const c = bytes[i];
    s += (c >= 32 && c <= 126) ? String.fromCharCode(c) : ' ';
  }
  return s.replace(/\s+/g, ' ').trim();
}

function walkJsFiles(rootDir) {
  const out = [];
  const stack = [rootDir];
  while (stack.length > 0) {
    const current = stack.pop();
    const entries = fs.readdirSync(current, { withFileTypes: true });
    for (const entry of entries) {
      const full = path.join(current, entry.name);
      if (entry.isDirectory()) {
        stack.push(full);
      } else if (entry.isFile() && full.endsWith('.js')) {
        out.push(full);
      }
    }
  }
  return out.sort();
}

function lineCollector() {
  const lines = [];
  const output = new Stream();
  output.write = function write(chunk, cb) {
    const isBuffer = BufferCtor && typeof BufferCtor.isBuffer === 'function' &&
      BufferCtor.isBuffer(chunk);
    lines.push(isBuffer ? chunk.toString('utf8') : String(chunk));
    if (typeof cb === 'function') cb(null);
    return true;
  };
  return { output, lines };
}

function summarizeBuiltinsTree(rootDir) {
  const jsFiles = walkJsFiles(rootDir);
  const topLevelCounts = new Map();
  let totalBytes = 0;
  let todoHits = 0;
  const sampleBuffers = [];

  for (const file of jsFiles) {
    const rel = path.relative(rootDir, file);
    const top = rel.split(path.sep)[0] || '.';
    topLevelCounts.set(top, (topLevelCounts.get(top) || 0) + 1);

    const data = fs.readFileSync(file);
    totalBytes += data.length;

    // Buffer-heavy aggregation so this sample exercises Buffer APIs in runtime.
    const text = fs.readFileSync(file, 'utf8');
    if (text.includes('TODO')) todoHits += 1;
    sampleBuffers.push(data.subarray(0, Math.min(24, data.length)));
  }

  const joined = concatBytes(sampleBuffers);
  const summaryBytes = joined.subarray(0, 96);
  let hexPreview = toHex(summaryBytes);
  if (BufferCtor && typeof BufferCtor.from === 'function') {
    const candidate = BufferCtor.from(summaryBytes).toString('hex');
    // Some lightweight Buffer shims may not support hex encoding correctly.
    if (typeof candidate === 'string' && candidate.length > 0 && !candidate.includes(',')) {
      hexPreview = candidate;
    }
  }
  const asciiPreview = toAsciiPreview(summaryBytes, 100);

  return {
    fileCount: jsFiles.length,
    totalBytes,
    todoHits,
    topLevelCounts: Object.fromEntries([...topLevelCounts.entries()].sort((a, b) => {
      if (a[0] < b[0]) return -1;
      if (a[0] > b[0]) return 1;
      return 0;
    })),
    hexPreview,
    asciiPreview,
  };
}

function main() {
  const runtimeBuiltinsDir = path.resolve(__dirname, '..', 'src', 'builtins');
  const reportDir = path.resolve(__dirname, '..', 'build', 'demo-output');
  fs.mkdirSync(reportDir, { recursive: true });

  const { output, lines } = lineCollector();
  const moduleStats = summarizeBuiltinsTree(runtimeBuiltinsDir);

  const runtimeInfo = {
    platform: os.platform(),
    arch: os.arch(),
    hostname: os.hostname(),
    release: os.release(),
    uptimeSec: Math.round(os.uptime()),
    cpus: os.cpus().length,
    parallelism: os.availableParallelism ? os.availableParallelism() : os.cpus().length,
    tmpdir: os.tmpdir(),
    netAutoSelectTimeoutMs: net.getDefaultAutoSelectFamilyAttemptTimeout(),
  };

  // Exercise net module surface.
  net.setDefaultAutoSelectFamilyAttemptTimeout(runtimeInfo.netAutoSelectTimeoutMs);

  const moduleInfo = {
    builtinModulesCount: Array.isArray(Module.builtinModules) ? Module.builtinModules.length : 0,
    builtinModulesSample: Array.isArray(Module.builtinModules) ? Module.builtinModules.slice(0, 12) : [],
  };

  const builtinsURL = pathToFileURL(runtimeBuiltinsDir);
  const report = {
    generatedAt: new Date().toISOString(),
    runtimeInfo,
    moduleInfo,
    builtinsPath: runtimeBuiltinsDir,
    builtinsFileURL: builtinsURL.href,
    moduleStats,
  };

  assert.strictEqual(typeof report.builtinsFileURL, 'string');
  assert.ok(report.moduleStats.fileCount > 0);

  const outputPath = path.join(reportDir, 'runtime-modules-report.json');
  fs.writeFileSync(outputPath, JSON.stringify(report, null, 2) + '\n', 'utf8');

  output.write(`Report written to: ${outputPath}\n`);
  output.write(`Runtime: ${runtimeInfo.platform}/${runtimeInfo.arch} cpus=${runtimeInfo.cpus}\n`);
  output.write(`Builtins scanned: ${moduleStats.fileCount} files, ${moduleStats.totalBytes} bytes\n`);
  output.write(`Builtin modules exposed: ${moduleInfo.builtinModulesCount}\n`);
  output.write(`Preview: ${moduleStats.asciiPreview}\n`);

  process.stdout.write(lines.join(''));
  process.stdout.write(`\nInspect report with util.inspect:\n${util.inspect(report, { depth: 3, colors: false })}\n`);
}

main();
