#!/usr/bin/env node
'use strict';

const fs = require('node:fs');
const path = require('node:path');

const fileArg = process.argv[2];
if (!fileArg) {
  console.error('usage: node scripts/summarize-results.js <results-json-file>');
  process.exit(1);
}

const abs = path.resolve(process.cwd(), fileArg);
const data = JSON.parse(fs.readFileSync(abs, 'utf8'));
const tests = Array.isArray(data.tests) ? data.tests : [];

const failed = tests.filter((t) => !t.compatible);

function pickReason(entry) {
  const text = [
    entry.baseline && entry.baseline.stderr,
    entry.edge && entry.edge.stderr,
    entry.baseline && entry.baseline.stdout,
    entry.edge && entry.edge.stdout,
  ].filter(Boolean).join('\n');

  const lines = text.split(/\r?\n/).map((l) => l.trim()).filter(Boolean);
  return lines.find((l) =>
    l.startsWith('Error') ||
    l.includes('ERR_') ||
    l.includes('Cannot find') ||
    l.includes('ReferenceError') ||
    l.includes('TypeError') ||
    l.includes('SyntaxError')
  ) || lines[0] || 'unknown';
}

console.log(`mode: ${data.mode}`);
console.log(`generatedAt: ${data.generatedAt}`);
console.log(`compatible: ${data.totals && data.totals.compatible}/${data.totals && data.totals.tests}`);
console.log(`failed: ${failed.length}`);

if (failed.length > 0) {
  console.log('\nFailures:');
  for (const entry of failed) {
    console.log(`- ${entry.test}: ${pickReason(entry)}`);
  }
}
