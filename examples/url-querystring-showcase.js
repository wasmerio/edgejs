'use strict';

const assert = require('assert');
const path = require('path');
const url = require('url');
const querystring = require('querystring');
const { URL, URLSearchParams } = url;

function section(title) {
  process.stdout.write(`\n=== ${title} ===\n`);
}

function printObject(label, value) {
  process.stdout.write(`${label}: ${JSON.stringify(value, null, 2)}\n`);
}

function legacyUrlDemo() {
  section('Legacy URL API (url.parse / url.format / url.resolve)');

  const source = 'https://user:pass@example.com:8080/a/b?x=1&y=two#frag';
  const parsed = url.parse(source, true);
  printObject('parsed', {
    protocol: parsed.protocol,
    auth: parsed.auth,
    host: parsed.host,
    pathname: parsed.pathname,
    query: parsed.query,
    hash: parsed.hash,
  });

  const formatted = url.format({
    protocol: 'https',
    slashes: true,
    hostname: 'api.example.com',
    pathname: '/v1/search',
    query: { q: 'node napi', page: 2 },
  });
  process.stdout.write(`formatted: ${formatted}\n`);

  const resolved = url.resolve('https://example.com/docs/guide/', '../api?lang=en');
  process.stdout.write(`resolved:  ${resolved}\n`);
}

function whatwgUrlDemo() {
  section('WHATWG URL + URLSearchParams');

  const u = new URL('https://example.org/products?category=books&sort=asc');
  const mutableParams = new URLSearchParams(String(u.search || '').replace(/^\?/, ''));
  mutableParams.set('page', '3');
  mutableParams.append('tag', 'new');
  u.search = `?${mutableParams.toString()}`;

  process.stdout.write(`href:      ${u.href}\n`);
  process.stdout.write(`origin:    ${u.origin}\n`);
  process.stdout.write(`pathname:  ${u.pathname}\n`);
  process.stdout.write(`query:     ${mutableParams.toString()}\n`);

  const params = new URLSearchParams('a=1&a=2&b=hello%20world');
  const pairs = [];
  for (const [k, v] of params.entries()) {
    pairs.push([k, v]);
  }
  printObject('params entries', pairs);
}

function querystringDemo() {
  section('querystring module');

  const encoded = querystring.stringify({
    q: 'napi v8',
    limit: 10,
    tags: ['url', 'querystring', 'demo'],
  });
  process.stdout.write(`stringify: ${encoded}\n`);

  const decoded = querystring.parse('q=napi%20v8&limit=10&tags=url&tags=querystring&tags=demo');
  printObject('parse', decoded);

  process.stdout.write(`escape:    ${querystring.escape('a value with spaces & symbols?')}\n`);
  process.stdout.write(`unescape:  ${querystring.unescape('hello%20world%21')}\n`);
}

function fileUrlDemo() {
  section('Path <-> file URL helpers');

  const here = path.resolve(__dirname, 'url-querystring-showcase.js');
  const file = url.pathToFileURL(here);
  const roundTrip = url.fileURLToPath(file);

  process.stdout.write(`path:      ${here}\n`);
  process.stdout.write(`file URL:  ${file.href}\n`);
  process.stdout.write(`toPath:    ${roundTrip}\n`);

  assert.strictEqual(roundTrip, here);
}

function main() {
  legacyUrlDemo();
  whatwgUrlDemo();
  querystringDemo();
  fileUrlDemo();

  process.stdout.write('\nDone.\n');
}

main();
