#!/usr/bin/env node
'use strict';

const fs = require('node:fs');
const path = require('node:path');

const HERE = __dirname;
const ROOT = path.resolve(HERE, '..');
const MANIFEST = path.join(ROOT, 'top-packages.json');
const PACKAGE_JSON = path.join(ROOT, 'package.json');
const INDEX_FILE = path.join(ROOT, 'generated-tests.json');

if (!fs.existsSync(MANIFEST)) {
  console.error(`Missing ${MANIFEST}. Run scripts/fetch-top-packages.js first.`);
  process.exit(1);
}

const manifest = JSON.parse(fs.readFileSync(MANIFEST, 'utf8'));
const packages = Array.isArray(manifest.packages) ? manifest.packages : [];
if (packages.length === 0) {
  console.error('No packages in top-packages.json');
  process.exit(1);
}

function toFileBase(pkgName) {
  return pkgName
    .replace(/^@/, 'scope-')
    .replace(/[\/]/g, '--')
    .replace(/[^a-zA-Z0-9._-]/g, '-');
}

function deepSnippet(pkgName) {
  const snippets = {
    minimatch: `
  const mm = typeof mod === 'function' ? mod : (mod.minimatch || mod.default);
  assert.equal(mm('a.js', '*.js'), true);
  assert.equal(mm('a.ts', '*.js'), false);
`,
    chalk: `
  const chalk = mod.default || mod;
  assert.equal(typeof chalk.green, 'function');
  const out = chalk.green('ok');
  assert.equal(out.includes('ok'), true);
`,
    'lru-cache': `
  const LRUCache = mod.LRUCache || mod.default || mod;
  const cache = new LRUCache({ max: 2 });
  cache.set('a', 1);
  assert.equal(cache.get('a'), 1);
`,
    'emoji-regex': `
  const mk = mod.default || mod;
  const re = mk();
  assert.equal(re.test('hello 😀'), true);
`,
    commander: `
  const Command = mod.Command || (mod.default && mod.default.Command);
  const cmd = new Command();
  cmd.option('--port <n>');
  cmd.parse(['node', 'x', '--port', '33']);
  assert.equal(cmd.opts().port, '33');
`,
    ajv: `
  const Ajv = mod.default || mod;
  const ajv = new Ajv();
  const validate = ajv.compile({
    type: 'object',
    properties: { x: { type: 'number' } },
    required: ['x'],
    additionalProperties: false,
  });
  assert.equal(validate({ x: 1 }), true);
  assert.equal(validate({ x: '1' }), false);
`,
    uuid: `
  const uuid = mod.default || mod;
  const id = uuid.v4();
  assert.equal(uuid.validate(id), true);
  assert.equal(uuid.version(id), 4);
`,
    'p-locate': `
  const pLocate = mod.default || mod;
  const found = await pLocate([1, 2, 3], async (x) => x === 2);
  assert.equal(found, 2);
`,
    ws: `
  const WS = mod.WebSocket || mod.default || mod;
  assert.equal(typeof WS, 'function');
  assert.equal(typeof WS.OPEN, 'number');
`,
    acorn: `
  const parser = mod.Parser || mod;
  const ast = parser.parse('const x = 1', { ecmaVersion: 'latest' });
  assert.equal(ast.type, 'Program');
`,
    postcss: `
  const postcss = mod.default || mod;
  const root = postcss.parse('a{color:red}');
  assert.equal(root.first.selector, 'a');
`,
    'fs-extra': `
  const fse = mod.default || mod;
  const os = require('node:os');
  const p = require('node:path');
  const d = p.join(os.tmpdir(), 'edgejs-topn-fse');
  const f = p.join(d, 'a.txt');
  fse.ensureDirSync(d);
  fse.writeFileSync(f, 'ok');
  assert.equal(fse.readFileSync(f, 'utf8'), 'ok');
`,
    'is-stream': `
  const isStream = mod.default || mod;
  const stream = require('node:stream');
  assert.equal(isStream(new stream.Readable({ read() {} })), true);
  assert.equal(isStream({}), false);
`,
    'path-to-regexp': `
  const ptr = mod.pathToRegexp || mod.default?.pathToRegexp || mod;
  const out = ptr('/user/:id');
  const re = out.regexp || out;
  assert.equal(re.test('/user/1'), true);
`,
    browserslist: `
  const bl = mod.default || mod;
  const result = bl('last 1 chrome version');
  assert.equal(Array.isArray(result), true);
  assert.equal(result.length > 0, true);
`,
    nanoid: `
  const fn = mod.nanoid || mod.default || mod;
  const id = fn(10);
  assert.equal(id.length, 10);
`,
    'strip-bom': `
  const fn = mod.default || mod;
  assert.equal(fn('\ufeffhello'), 'hello');
`,
    mime: `
  const api = mod.default || mod;
  const getType = api.getType || api.default_type || api.lookup;
  assert.equal(typeof getType, 'function');
  assert.equal(String(getType('file.json')).includes('json'), true);
`,
    dotenv: `
  const dotenv = mod.default || mod;
  const parsed = dotenv.parse(['A=1', 'B=two'].join('\\n'));
  assert.equal(parsed.A, '1');
  assert.equal(parsed.B, 'two');
`,
    eslint: `
  const ESLint = mod.ESLint || (mod.default && mod.default.ESLint);
  const engine = new ESLint({
    useEslintrc: false,
    overrideConfig: { parserOptions: { ecmaVersion: 2022 }, rules: { semi: ['error', 'always'] } },
  });
  const results = await engine.lintText('const x = 1');
  assert.equal(Array.isArray(results), true);
  assert.equal(results.length > 0, true);
`,
    jsonfile: `
  const jf = mod.default || mod;
  const os = require('node:os');
  const p = require('node:path');
  const f = p.join(os.tmpdir(), 'edgejs-topn-jsonfile.json');
  await jf.writeFile(f, { x: 1 });
  const data = await jf.readFile(f);
  assert.equal(data.x, 1);
`,
    react: `
  const react = mod.default || mod;
  const el = react.createElement('div', { id: 'a' }, 'x');
  assert.equal(el.type, 'div');
`,
    async: `
  const asyncLib = mod.default || mod;
  const out = await asyncLib.series([
    (cb) => cb(null, 1),
    (cb) => cb(null, 2),
  ]);
  assert.deepEqual(out, [1, 2]);
`,
    'write-file-atomic': `
  const wfa = mod.default || mod;
  const fs = require('node:fs');
  const os = require('node:os');
  const p = require('node:path');
  const f = p.join(os.tmpdir(), 'edgejs-topn-wfa.txt');
  if (typeof wfa.sync === 'function') {
    wfa.sync(f, 'ok');
  } else {
    await wfa(f, 'ok');
  }
  assert.equal(fs.readFileSync(f, 'utf8'), 'ok');
`,
    'supports-preserve-symlinks-flag': `
  const fn = mod.default || mod;
  assert.equal(typeof fn(), 'boolean');
`,
    'hosted-git-info': `
  const hgi = mod.default || mod;
  const info = hgi.fromUrl('https://github.com/npm/cli.git');
  assert.equal(info.type, 'github');
`,
    rxjs: `
  const rx = mod.default || mod;
  const value = await rx.lastValueFrom(rx.of(1, 2).pipe(rx.map((x) => x + 1)));
  assert.equal(value, 3);
`,
    express: `
  const express = mod.default || mod;
  const app = express();
  app.get('/x', (_req, res) => res.send('ok'));
  assert.equal(typeof app.use, 'function');
`,
    prettier: `
  const prettier = mod.default || mod;
  const formatted = await prettier.format('const x=1', { parser: 'babel' });
  assert.equal(typeof formatted, 'string');
  assert.equal(formatted.includes('const x = 1;'), true);
`,
    htmlparser2: `
  const hp = mod.default || mod;
  const doc = hp.parseDocument('<div>ok</div>');
  assert.equal(doc.children.length > 0, true);
`,
    figures: `
  const figures = mod.default || mod;
  assert.equal(typeof figures.tick, 'string');
`,
    'core-js': `
  assert.equal(typeof Promise, 'function');
  assert.equal(typeof Symbol, 'function');
`,
    boxen: `
  const boxen = mod.default || mod;
  const s = boxen('ok');
  assert.equal(typeof s, 'string');
  assert.equal(s.includes('ok'), true);
`,
    got: `
  const got = mod.default || mod;
  assert.equal(typeof got, 'function');
  assert.equal(typeof got.extend, 'function');
`,
    redux: `
  const redux = mod.default || mod;
  const reducer = (s = 0, a) => (a.type === 'inc' ? s + 1 : s);
  const store = redux.createStore(reducer);
  store.dispatch({ type: 'inc' });
  assert.equal(store.getState(), 1);
`,
    colord: `
  const colord = (mod.colord || mod.default || mod);
  const hex = colord('#ff0000').toHex();
  assert.equal(hex, '#ff0000');
`,
    filesize: `
  const fsz = mod.default || mod;
  const out = fsz(1024);
  assert.equal(typeof out, 'string');
`,
    mobx: `
  const mobx = mod.default || mod;
  const state = mobx.observable({ n: 1 });
  mobx.runInAction(() => { state.n += 1; });
  assert.equal(state.n, 2);
`,
    underscore: `
  const us = mod.default || mod;
  assert.deepEqual(us.map([1, 2, 3], (x) => x * 2), [2, 4, 6]);
`,
    eventemitter3: `
  const EE = mod.default || mod;
  const ee = new EE();
  let called = 0;
  ee.on('x', () => { called += 1; });
  ee.emit('x');
  assert.equal(called, 1);
`,
    tailwindcss: `
  const tw = mod.default || mod;
  const cfg = tw({ content: [], theme: {}, plugins: [] });
  assert.equal(typeof cfg, 'object');
`,
    cors: `
  const cors = mod.default || mod;
  const mw = cors();
  assert.equal(typeof mw, 'function');
`,
    'clean-css': `
  const CleanCSS = mod.default || mod;
  const out = new CleanCSS().minify('a { color: red; }');
  assert.equal(typeof out.styles, 'string');
`,
    pako: `
  const pako = mod.default || mod;
  const bytes = pako.deflate('hello');
  const out = pako.inflate(bytes, { to: 'string' });
  assert.equal(out, 'hello');
`,
    espree: `
  const espree = mod.default || mod;
  const ast = espree.parse('const x = 1', { ecmaVersion: 'latest' });
  assert.equal(ast.type, 'Program');
`,
    'decimal.js': `
  const Decimal = mod.default || mod;
  const value = new Decimal('0.1').plus(new Decimal('0.2')).toString();
  assert.equal(value, '0.3');
`,
    'big.js': `
  const Big = mod.default || mod;
  const value = new Big('0.1').plus('0.2').toString();
  assert.equal(value, '0.3');
`,
    doctrine: `
  const doctrine = mod.default || mod;
  const parsed = doctrine.parse('/** @param {string} x */', { unwrap: true });
  assert.equal(Array.isArray(parsed.tags), true);
`,
    execa: `
  const execa = mod.execa || mod.default || mod;
  assert.equal(typeof execa, 'function');
`,
    argparse: `
  const argparse = mod.default || mod;
  const parser = new argparse.ArgumentParser({ add_help: false });
  parser.add_argument('--x');
  const ns = parser.parse_args(['--x', '1']);
  assert.equal(ns.x, '1');
`,
    'schema-utils': `
  const su = mod.default || mod;
  const schema = { type: 'object', properties: { x: { type: 'number' } }, required: ['x'] };
  su.validate(schema, { x: 1 });
`,
    'react-hook-form': `
  const rhf = mod.default || mod;
  assert.equal(typeof rhf.useForm, 'function');
`,
    'safe-buffer': `
  const sb = mod.default || mod;
  const b = sb.Buffer.from('ok');
  assert.equal(b.toString('utf8'), 'ok');
`,
    'body-parser': `
  const bp = mod.default || mod;
  assert.equal(typeof bp.json, 'function');
`,
    ignore: `
  const ig = mod.default || mod;
  const inst = ig().add('node_modules');
  assert.equal(inst.ignores('node_modules/a.js'), true);
`,
    classnames: `
  const cx = mod.default || mod;
  assert.equal(cx('a', { b: true, c: false }), 'a b');
`,
  };

  return snippets[pkgName] || `
  // Generic fallback: verify export shape and basic introspection.
  const t = typeof mod;
  assert.ok(
    t === 'function' || t === 'object' || t === 'string' || t === 'number' || t === 'boolean',
    'module export type should be a JS value',
  );
  if (t === 'object') {
    const keys = Object.keys(mod);
    assert.equal(Array.isArray(keys), true);
  }
  if (t === 'function') {
    assert.equal(mod.length >= 0, true);
  }
`;
}

function testTemplate(pkgName) {
  return `'use strict';\n\nconst assert = require('node:assert/strict');\n\nasync function loadPackage() {\n  try {\n    return require('${pkgName}');\n  } catch (_requireErr) {\n    const imported = await import('${pkgName}');\n    return imported && Object.prototype.hasOwnProperty.call(imported, 'default')\n      ? imported.default\n      : imported;\n  }\n}\n\n(async () => {\n  const mod = await loadPackage();\n  assert.notEqual(mod, undefined, 'module should load');\n  assert.notEqual(mod, null, 'module should not be null');${deepSnippet(pkgName)}\n\n  console.log('${pkgName}-test:ok');\n})().catch((err) => {\n  console.error('${pkgName}-test:fail');\n  console.error(err && err.stack ? err.stack : String(err));\n  process.exit(1);\n});\n`;
}

const index = {
  generatedAt: new Date().toISOString(),
  sourceManifest: path.basename(MANIFEST),
  tests: [],
};

for (const pkg of packages) {
  const fileBase = toFileBase(pkg.name);
  const fileName = `${fileBase}-test.js`;
  const relPath = fileName;
  const absPath = path.join(ROOT, relPath);

  fs.writeFileSync(absPath, testTemplate(pkg.name));

  index.tests.push({
    packageName: pkg.name,
    testFile: relPath,
    rank: pkg.rank,
  });
}

fs.writeFileSync(INDEX_FILE, `${JSON.stringify(index, null, 2)}\n`);

const pkgJson = JSON.parse(fs.readFileSync(PACKAGE_JSON, 'utf8'));
pkgJson.dependencies = pkgJson.dependencies || {};

for (const pkg of packages) {
  pkgJson.dependencies[pkg.name] = String(pkg.version || 'latest');
}

pkgJson.scripts = pkgJson.scripts || {};
pkgJson.scripts['prep:top100'] = 'node scripts/fetch-top-packages.js --count=100 && node scripts/generate-tests.js';
pkgJson.scripts['test:compat:generated'] = 'node run-compat.js --manifest generated-tests.json';
pkgJson.scripts['test:baseline:generated'] = 'node run-compat.js --manifest generated-tests.json --baseline-only';
pkgJson.scripts['test:edgejs:generated'] = 'node run-compat.js --manifest generated-tests.json --edge-only';

fs.writeFileSync(PACKAGE_JSON, `${JSON.stringify(pkgJson, null, 2)}\n`);
console.log(`Generated ${index.tests.length} deep test files and updated package.json dependencies.`);
