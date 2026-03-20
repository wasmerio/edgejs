'use strict';

const assert = require('node:assert/strict');

(async () => {
  const mod = await import('emoji-regex');
  const emojiRegex = mod.default;

  // The export is a function that returns a RegExp
  assert.equal(typeof emojiRegex, 'function');
  const re = emojiRegex();
  assert.ok(re instanceof RegExp);

  // Matches common emoji
  assert.ok(re.test('\u{1F600}'), 'should match grinning face');

  // Reset lastIndex since the regex is global
  const re2 = emojiRegex();
  assert.ok(re2.test('\u{1F680}'), 'should match rocket');

  // Match multiple emoji in a string
  const text = 'Hello \u{1F600} World \u{1F680}\u{2764}\u{FE0F}';
  const matches = text.match(emojiRegex());
  assert.ok(matches !== null, 'should find emoji in mixed text');
  assert.ok(matches.length >= 3, 'should find at least 3 emoji');

  // No false positives on regular ASCII text
  const plainText = 'Hello, World! 12345 foo@bar.com #hashtag';
  const plainMatches = plainText.match(emojiRegex());
  assert.equal(plainMatches, null, 'should not match regular text');

  // No false positives on common punctuation and symbols
  const punctuation = '!@#$%^&*()_+-=[]{}|;:,.<>?/~`';
  assert.equal(punctuation.match(emojiRegex()), null, 'should not match punctuation');

  // Multi-byte / surrogate pair emoji
  const complexEmoji = '\u{1F468}\u{200D}\u{1F469}\u{200D}\u{1F467}\u{200D}\u{1F466}'; // family emoji
  const complexMatches = complexEmoji.match(emojiRegex());
  assert.ok(complexMatches !== null, 'should match family emoji sequence');

  // Flag emoji (regional indicators)
  const flag = '\u{1F1FA}\u{1F1F8}'; // US flag
  const flagMatches = flag.match(emojiRegex());
  assert.ok(flagMatches !== null, 'should match flag emoji');

  // Skin tone modifier emoji
  const skinTone = '\u{1F44B}\u{1F3FD}'; // waving hand medium skin tone
  const skinMatches = skinTone.match(emojiRegex());
  assert.ok(skinMatches !== null, 'should match skin tone emoji');

  // Can be used to strip emoji from text
  const cleaned = 'Hello \u{1F600} World'.replace(emojiRegex(), '').trim();
  assert.ok(!cleaned.match(emojiRegex()), 'stripped text should have no emoji');
  assert.ok(cleaned.includes('Hello'), 'stripped text should keep non-emoji content');

  console.log('emoji-regex-test:ok');
})().catch((err) => {
  console.error('emoji-regex-test:fail');
  console.error(err && err.stack ? err.stack : String(err));
  process.exit(1);
});
