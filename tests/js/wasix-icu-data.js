'use strict';

// ICU data smoke test for WASIX builds that read ICU's individual data items
// from the filesystem (/usr/share/edge-icu/icudt78l/...) instead of embedding
// the common data blob (EDGE_EMBED_ICU_DATA=OFF). Each check below needs a
// different kind of ICU item to be found and opened at runtime:
//
//   - legacy charset converters (*.cnv) for TextDecoder;
//   - locale collation rules (coll/*.res);
//   - locale names and calendar data (*.res) plus zoneinfo64.res;
//   - plural rules (plurals.res), break-iterator dictionaries (brkitr/);
//   - likely-subtags data for Intl.Locale#maximize.
//
// A missing or unreadable data tree degrades these to root-locale / ASCII
// behaviour instead of throwing, so every check asserts the localized result.
// Self-contained: prints one PASS line and exits non-zero on any failure.

const assert = require('node:assert');

const failures = [];
let passed = 0;

function check(name, fn) {
  try {
    fn();
    passed++;
  } catch (err) {
    failures.push(`${name}: ${err && (err.stack || err.message || err)}`);
  }
}

function decode(encoding, bytes) {
  return new TextDecoder(encoding).decode(new Uint8Array(bytes));
}

// Legacy multi-byte encodings come from ICU converter tables (*.cnv). Single-
// byte windows-1252 is deliberately not used: it is served by JS tables.
check('TextDecoder shift_jis', () => {
  // "日本" + "あ"
  assert.strictEqual(decode('shift_jis', [0x93, 0xfa, 0x96, 0x7b, 0x82, 0xa0]), '日本あ');
});

check('TextDecoder gb18030', () => {
  // "中文" (two-byte) + U+0080 (four-byte sequence 81 30 81 30)
  assert.strictEqual(decode('gb18030', [0xd6, 0xd0, 0xce, 0xc4, 0x81, 0x30, 0x81, 0x30]),
                     '中文\u0080');
});

check('TextDecoder big5', () => {
  // "中文"
  assert.strictEqual(decode('big5', [0xa4, 0xa4, 0xa4, 0xe5]), '中文');
});

// German and Swedish place "ä" differently; root collation agrees with German,
// so Swedish proves the tailoring data was loaded.
check('Intl.Collator de ordering', () => {
  const sorted = ['z', 'ä', 'a'].sort(new Intl.Collator('de').compare);
  assert.deepStrictEqual(sorted, ['a', 'ä', 'z']);
});

check('Intl.Collator sv ordering', () => {
  const sorted = ['ä', 'z', 'a'].sort(new Intl.Collator('sv').compare);
  assert.deepStrictEqual(sorted, ['a', 'z', 'ä']);
  assert.ok('ä'.localeCompare('z', 'sv') > 0);
});

check('Intl.DateTimeFormat de month name (UTC)', () => {
  const fmt = new Intl.DateTimeFormat('de', { month: 'long', timeZone: 'UTC' });
  assert.strictEqual(fmt.format(new Date(0)), 'Januar');
  const may = new Intl.DateTimeFormat('de', { month: 'long', timeZone: 'UTC' });
  assert.strictEqual(may.format(Date.UTC(2024, 4, 15)), 'Mai');
});

check('Intl.DateTimeFormat explicit timeZone', () => {
  const fmt = new Intl.DateTimeFormat('en-US', {
    timeZone: 'Asia/Tokyo',
    hour: 'numeric',
    hourCycle: 'h23',
  });
  const hour = fmt.formatToParts(new Date(0)).find((part) => part.type === 'hour');
  assert.ok(hour, 'missing hour part');
  assert.strictEqual(Number(hour.value), 9);
  assert.strictEqual(fmt.resolvedOptions().timeZone, 'Asia/Tokyo');
});

check('Intl.DateTimeFormat default timeZone', () => {
  const zone = new Intl.DateTimeFormat().resolvedOptions().timeZone;
  assert.strictEqual(typeof zone, 'string');
  assert.ok(zone.length > 0, 'empty default timeZone');
  // Formatting in the default zone must still produce localized output.
  assert.strictEqual(typeof new Date(0).toLocaleString('de-DE'), 'string');
});

check('Intl.NumberFormat de grouping', () => {
  assert.strictEqual(new Intl.NumberFormat('de-DE').format(1234567.891), '1.234.567,891');
});

check('Intl.PluralRules ar categories', () => {
  const rules = new Intl.PluralRules('ar');
  const expected = ['few', 'many', 'one', 'other', 'two', 'zero'];
  // resolvedOptions().pluralCategories is not reported by every engine's Intl
  // surface (QuickJS omits it); the categories reached by select() are.
  const reported = rules.resolvedOptions().pluralCategories;
  if (reported !== undefined) assert.deepStrictEqual(reported.slice().sort(), expected);
  const reached = new Set();
  for (let n = 0; n <= 110; n++) reached.add(rules.select(n));
  assert.deepStrictEqual(Array.from(reached).sort(), expected);
  assert.strictEqual(rules.select(0), 'zero');
  assert.strictEqual(rules.select(1), 'one');
  assert.strictEqual(rules.select(2), 'two');
  assert.strictEqual(rules.select(3), 'few');
  assert.strictEqual(rules.select(11), 'many');
  assert.strictEqual(rules.select(100), 'other');
});

check('Intl.Segmenter ja words', () => {
  if (typeof Intl.Segmenter !== 'function') return;
  const input = '日本語のテキストです';
  const segments = Array.from(
    new Intl.Segmenter('ja', { granularity: 'word' }).segment(input),
    (entry) => entry.segment);
  assert.strictEqual(segments.join(''), input);
  // Without the CJK dictionary the whole run is a single segment.
  assert.ok(segments.length >= 3, `expected dictionary word breaks, got ${JSON.stringify(segments)}`);
});

check('supportedLocalesOf', () => {
  assert.deepStrictEqual(Intl.Collator.supportedLocalesOf(['de', 'sv']), ['de', 'sv']);
  assert.deepStrictEqual(Intl.DateTimeFormat.supportedLocalesOf(['ja', 'ar']), ['ja', 'ar']);
  assert.deepStrictEqual(Intl.NumberFormat.supportedLocalesOf(['de-DE']), ['de-DE']);
});

check('Intl.Locale maximize', () => {
  if (typeof Intl.Locale !== 'function') throw new Error('Intl.Locale is missing');
  assert.strictEqual(new Intl.Locale('zh').maximize().toString(), 'zh-Hans-CN');
  assert.strictEqual(new Intl.Locale('sr').maximize().toString(), 'sr-Cyrl-RS');
  assert.strictEqual(new Intl.Locale('und-Latn-DE').maximize().language, 'de');
});

if (failures.length > 0) {
  console.error(`FAIL wasix-icu-data: ${failures.length} failed, ${passed} passed`);
  for (const failure of failures) console.error(`  - ${failure}`);
  process.exitCode = 1;
} else {
  console.log(`PASS wasix-icu-data: ${passed} ICU data checks`);
}
