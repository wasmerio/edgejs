#!/usr/bin/env node
'use strict';

const fs = require('node:fs');
const path = require('node:path');
const cp = require('node:child_process');

const here = __dirname;
const repoRoot = path.resolve(here, '..');
const runSh = path.join(repoRoot, 'run.sh');
const wasmPath = 'build-wasix/ubi.wasm';
const resultsDir = path.join(here, 'results');

const args = process.argv.slice(2);
const baselineOnly = args.includes('--baseline-only');
const edgeOnly = args.includes('--edge-only');
const manifestArg = args.find((arg) => arg.startsWith('--manifest='));
const manifestFlagIndex = args.indexOf('--manifest');
const manifestFile = manifestArg
  ? manifestArg.slice('--manifest='.length)
  : (manifestFlagIndex >= 0 ? args[manifestFlagIndex + 1] : null);

if (baselineOnly && edgeOnly) {
  console.error('Cannot use --baseline-only and --edge-only together.');
  process.exit(2);
}

const requested = args.filter((arg, index) => {
  if (arg.startsWith('--')) return false;
  if (manifestFlagIndex >= 0 && index === manifestFlagIndex + 1) return false;
  return true;
});

function normalizeRequestedName(name) {
  if (name.endsWith('.js')) return name;
  if (name.endsWith('-test')) return `${name}.js`;
  return `${name}-test.js`;
}

function loadManifestTests(fileName) {
  const manifestPath = path.resolve(here, fileName);
  const parsed = JSON.parse(fs.readFileSync(manifestPath, 'utf8'));
  const entries = Array.isArray(parsed.tests) ? parsed.tests : [];
  return entries.map((entry) => {
    if (typeof entry === 'string') return entry;
    if (entry && typeof entry.testFile === 'string') return entry.testFile;
    return '';
  }).filter(Boolean);
}

const discovered = manifestFile
  ? loadManifestTests(manifestFile)
  : fs
    .readdirSync(here)
    .filter((name) => name.endsWith('-test.js'))
    .sort();

const tests = requested.length > 0
  ? requested.map(normalizeRequestedName)
  : discovered;

if (tests.length === 0) {
  console.error('No test files found in top-n-test/.');
  process.exit(1);
}

if (!fs.existsSync(runSh) && !baselineOnly) {
  console.error(`Missing run.sh at ${runSh}`);
  process.exit(1);
}

fs.mkdirSync(resultsDir, { recursive: true });

function runCommand(cmd, cmdArgs, cwd) {
  const started = Date.now();
  const result = cp.spawnSync(cmd, cmdArgs, {
    cwd,
    encoding: 'utf8',
    stdio: ['ignore', 'pipe', 'pipe'],
    env: process.env,
  });
  return {
    ok: result.status === 0,
    status: result.status,
    signal: result.signal,
    ms: Date.now() - started,
    stdout: result.stdout || '',
    stderr: result.stderr || '',
    command: [cmd, ...cmdArgs].join(' '),
  };
}

const summary = {
  generatedAt: new Date().toISOString(),
  mode: baselineOnly ? 'baseline-only' : edgeOnly ? 'edge-only' : 'baseline+edge',
  totals: {
    tests: tests.length,
    baselinePassed: 0,
    edgePassed: 0,
    compatible: 0,
    failed: 0,
  },
  tests: [],
};

let hadFailure = false;

for (const file of tests) {
  const abs = path.resolve(here, file);
  if (!fs.existsSync(abs)) {
    summary.tests.push({
      test: file,
      error: 'missing test file',
      compatible: false,
    });
    hadFailure = true;
    continue;
  }

  const entry = { test: file };

  if (!edgeOnly) {
    entry.baseline = runCommand('node', [abs], here);
    if (entry.baseline.ok) {
      summary.totals.baselinePassed += 1;
    }
  }

  if (!baselineOnly) {
    const relToRepo = path.relative(repoRoot, abs).replace(/\\/g, '/');
    const scriptArg = relToRepo;
    entry.edge = runCommand(runSh, [wasmPath, scriptArg], repoRoot);
    if (entry.edge.ok) {
      summary.totals.edgePassed += 1;
    }
  }

  const baselineOk = edgeOnly ? true : !!entry.baseline?.ok;
  const edgeOk = baselineOnly ? true : !!entry.edge?.ok;
  entry.compatible = baselineOk && edgeOk;

  if (entry.compatible) {
    summary.totals.compatible += 1;
  } else {
    summary.totals.failed += 1;
    hadFailure = true;
  }

  summary.tests.push(entry);

  const status = entry.compatible ? 'PASS' : 'FAIL';
  console.log(`${status} ${file}`);
}

const latestPath = path.join(resultsDir, 'latest.json');
const stampedPath = path.join(
  resultsDir,
  `compat-${new Date().toISOString().replace(/[.:]/g, '-')}.json`,
);
fs.writeFileSync(latestPath, `${JSON.stringify(summary, null, 2)}\n`);
fs.writeFileSync(stampedPath, `${JSON.stringify(summary, null, 2)}\n`);

console.log(`\nSummary: ${summary.totals.compatible}/${summary.totals.tests} compatible`);
console.log(`Report: ${latestPath}`);

process.exit(hadFailure ? 1 : 0);
