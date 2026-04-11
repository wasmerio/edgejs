#!/usr/bin/env node

import { spawnSync } from 'node:child_process';
import { writeFile } from 'node:fs/promises';
import process from 'node:process';

function usage() {
  console.error(
      'Usage: capture-edge-startup-profile.mjs <json-out> <markdown-out> <command>');
  process.exit(1);
}

function isStartupProfileObject(value) {
  if (value == null || typeof value !== 'object' || Array.isArray(value)) return false;
  const keys = Object.keys(value);
  if (keys.length === 0) return false;
  return keys.some((key) => {
    const normalized = key.toLowerCase();
    return normalized.includes('bootstrap') || normalized.includes('startup') ||
        normalized.includes('profile');
  });
}

function extractProfileJson(stdout, stderr) {
  const lines = `${stdout ?? ''}\n${stderr ?? ''}`
      .split(/\r?\n/)
      .map((line) => line.trim())
      .filter((line) => line.startsWith('{') && line.endsWith('}'));

  for (let i = lines.length - 1; i >= 0; i -= 1) {
    try {
      const parsed = JSON.parse(lines[i]);
      if (isStartupProfileObject(parsed)) {
        return parsed;
      }
    } catch {
      // Ignore non-JSON lines.
    }
  }
  return null;
}

function formatValue(value) {
  if (typeof value !== 'number') return String(value);
  if (Number.isInteger(value)) return String(value);
  return value.toFixed(3);
}

function renderMarkdown(command, profile) {
  if (profile.enabled === false) {
    return [
      '# Edge Startup Profile',
      '',
      `Command: \`${command}\``,
      '',
      'Startup profiling output was not detected for this run.',
      '',
      'The harness set `EDGE_STARTUP_PROFILE=1`, but no startup profile JSON was emitted.',
      '',
    ].join('\n');
  }
  const rows = Object.entries(profile).map(
      ([key, value]) => `| \`${key}\` | ${formatValue(value)} |`);
  return [
    '# Edge Startup Profile',
    '',
    `Command: \`${command}\``,
    '',
    '| Metric | Value |',
    '| --- | ---: |',
    ...rows,
    '',
  ].join('\n');
}

async function main() {
  const [, , jsonOut, markdownOut, command] = process.argv;
  if (!jsonOut || !markdownOut || !command) usage();

  const result = spawnSync(command, {
    shell: true,
    encoding: 'utf8',
    env: {
      ...process.env,
      EDGE_STARTUP_PROFILE: '1',
    },
  });

  if (result.error) {
    throw result.error;
  }

  if (result.status !== 0) {
    if (result.stdout) process.stdout.write(result.stdout);
    if (result.stderr) process.stderr.write(result.stderr);
    process.exit(result.status ?? 1);
  }

  const extractedProfile = extractProfileJson(result.stdout ?? '', result.stderr ?? '');
  const profile = extractedProfile == null ?
    {
      enabled: false,
      reason: 'EDGE_STARTUP_PROFILE did not emit startup profile JSON on this checkout',
    } :
    extractedProfile;

  await writeFile(jsonOut, `${JSON.stringify(profile, null, 2)}\n`);
  await writeFile(markdownOut, renderMarkdown(command, profile));
}

main().catch((error) => {
  console.error(error instanceof Error ? error.message : String(error));
  process.exit(1);
});
