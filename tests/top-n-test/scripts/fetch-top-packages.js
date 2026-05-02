#!/usr/bin/env node
'use strict';

const fs = require('node:fs');
const path = require('node:path');
const https = require('node:https');

const HERE = __dirname;
const ROOT = path.resolve(HERE, '..');
const OUT_FILE = path.join(ROOT, 'top-packages.json');

const args = process.argv.slice(2);
const countArg = args.find((a) => a.startsWith('--count='));
const candidatesArg = args.find((a) => a.startsWith('--candidate-limit='));
const COUNT = countArg ? Number(countArg.split('=')[1]) : 100;
const CANDIDATE_LIMIT = candidatesArg ? Number(candidatesArg.split('=')[1]) : 600;
const ALLOW_UPDATE = args.includes('--allow-update');

if (!Number.isInteger(COUNT) || COUNT <= 0) {
  console.error('Invalid --count value.');
  process.exit(2);
}

function getJson(url) {
  return new Promise((resolve, reject) => {
    const req = https.get(url, {
      headers: {
        'User-Agent': 'edgejs-top-n-test-inventory/0.3',
        'Accept': 'application/json',
      },
    }, (res) => {
      if (res.statusCode < 200 || res.statusCode >= 300) {
        reject(new Error(`GET ${url} -> ${res.statusCode}`));
        res.resume();
        return;
      }
      let raw = '';
      res.setEncoding('utf8');
      res.on('data', (c) => { raw += c; });
      res.on('end', () => {
        try {
          resolve(JSON.parse(raw));
        } catch (e) {
          reject(new Error(`Invalid JSON from ${url}: ${e.message}`));
        }
      });
    });
    req.on('error', reject);
  });
}

function sleep(ms) {
  return new Promise((r) => setTimeout(r, ms));
}

function shouldIncludePackage(name) {
  if (!name || typeof name !== 'string') return false;
  if (name.startsWith('@types/')) return false;
  if (name.includes('example')) return false;
  if (name.includes('demo')) return false;
  if (name.includes('template')) return false;
  if (name === 'bootstrap') return false; // browser-first package; out of Node runtime inventory scope
  return true;
}

async function fetchCandidates() {
  const queries = [
    { q: 'scope:unscoped', offsets: [0, 250, 500, 750] },
    { q: 'scope:scoped', offsets: [0, 250, 500] },
  ];

  const byName = new Map();

  for (const q of queries) {
    for (const from of q.offsets) {
      const url = `https://api.npms.io/v2/search?q=${encodeURIComponent(q.q)}&size=250&from=${from}`;
      try {
        const data = await getJson(url);
        const results = Array.isArray(data.results) ? data.results : [];
        for (const result of results) {
          const p = result.package || {};
          if (!shouldIncludePackage(p.name)) continue;
          if (!byName.has(p.name)) {
            byName.set(p.name, {
              name: p.name,
              version: p.version || 'latest',
              description: p.description || '',
              sourceQuery: q.q,
            });
          }
          if (byName.size >= CANDIDATE_LIMIT) {
            return [...byName.values()];
          }
        }
      } catch (err) {
        console.warn(`warn: candidate query failed q=${q.q} from=${from}: ${err.message}`);
      }
      await sleep(180);
    }
  }

  return [...byName.values()];
}

async function fetchDownloadsSingle(pkgName, attempt = 1) {
  const encoded = encodeURIComponent(pkgName);
  const url = `https://api.npmjs.org/downloads/point/last-year/${encoded}`;
  try {
    const data = await getJson(url);
    if (data && typeof data.downloads === 'number') return data.downloads;
    return 0;
  } catch (err) {
    if (attempt < 3) {
      await sleep(220 * attempt);
      return fetchDownloadsSingle(pkgName, attempt + 1);
    }
    return 0;
  }
}

async function enrichDownloads(candidates) {
  const concurrency = 8;
  const out = [];
  let cursor = 0;

  async function worker() {
    while (true) {
      const idx = cursor;
      cursor += 1;
      if (idx >= candidates.length) return;
      const item = candidates[idx];
      const downloadsLastYear = await fetchDownloadsSingle(item.name);
      out[idx] = { ...item, downloadsLastYear };
      if ((idx + 1) % 25 === 0 || idx + 1 === candidates.length) {
        console.log(`downloads: ${idx + 1}/${candidates.length}`);
      }
    }
  }

  await Promise.all(Array.from({ length: concurrency }, () => worker()));
  return out.filter(Boolean);
}

(async () => {
  if (fs.existsSync(OUT_FILE)) {
    try {
      const existing = JSON.parse(fs.readFileSync(OUT_FILE, 'utf8'));
      if (existing && existing.locked === true && !ALLOW_UPDATE) {
        console.error([
          `Refusing to overwrite locked inventory: ${OUT_FILE}`,
          'Set --allow-update only when performing an intentional manual refresh.',
        ].join('\n'));
        process.exit(2);
      }
    } catch (_err) {
      // If existing content is unreadable JSON, continue and overwrite.
    }
  }

  const candidates = await fetchCandidates();
  if (candidates.length === 0) {
    console.error('No candidates found.');
    process.exit(1);
  }

  console.log(`candidate packages: ${candidates.length}`);
  const enriched = await enrichDownloads(candidates);

  const selected = enriched
    .sort((a, b) => b.downloadsLastYear - a.downloadsLastYear)
    .slice(0, COUNT)
    .map((pkg, idx) => ({
      rank: idx + 1,
      name: pkg.name,
      version: pkg.version || 'latest',
      downloadsLastYear: pkg.downloadsLastYear,
      description: pkg.description,
    }));

  const output = {
    generatedAt: new Date().toISOString(),
    method: {
      note: 'Top-N inventory derived from npms candidate pool and ranked by npm last-year downloads.',
      candidateQueries: ['scope:unscoped', 'scope:scoped'],
      candidateSource: 'https://api.npms.io/v2/search',
      rankingSource: 'https://api.npmjs.org/downloads/point/last-year/<package>',
      caveat: 'Inventory-oriented approximation from broad candidate pool; locked for reproducibility.',
    },
    countRequested: COUNT,
    candidateCount: candidates.length,
    countSelected: selected.length,
    packages: selected,
    locked: true,
    updatePolicy: 'manual-only',
  };

  fs.writeFileSync(OUT_FILE, `${JSON.stringify(output, null, 2)}\n`);
  console.log(`Wrote ${selected.length} packages to ${OUT_FILE}`);
})();
