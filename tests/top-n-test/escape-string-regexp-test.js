'use strict';

const assert = require('node:assert/strict');

(async () => {
  const mod = await import('escape-string-regexp');
  const escapeStringRegexp = mod.default;

  assert.equal(typeof escapeStringRegexp, 'function');

  // Escaping dots: a dot in regex matches any char, escaped it matches literal dot
  const escaped = escapeStringRegexp('file.txt');
  const re = new RegExp('^' + escaped + '$');
  assert.ok(re.test('file.txt'), 'should match literal dot');
  assert.ok(!re.test('fileXtxt'), 'escaped dot should not match arbitrary char');

  // Escaping brackets
  const brackets = escapeStringRegexp('[test]');
  const reBrackets = new RegExp('^' + brackets + '$');
  assert.ok(reBrackets.test('[test]'));
  assert.ok(!reBrackets.test('t'));  // would match unescaped [test]

  // Escaping parentheses
  const parens = escapeStringRegexp('(group)');
  const reParens = new RegExp('^' + parens + '$');
  assert.ok(reParens.test('(group)'));
  assert.ok(!reParens.test('group'));

  // Escaping asterisks
  const star = escapeStringRegexp('a*b');
  const reStar = new RegExp('^' + star + '$');
  assert.ok(reStar.test('a*b'));
  assert.ok(!reStar.test('aaab'));  // unescaped * would allow this

  // Escaping plus, question mark, caret, dollar
  const mixed = escapeStringRegexp('^start+end$');
  const reMixed = new RegExp('^' + mixed + '$');
  assert.ok(reMixed.test('^start+end$'));
  assert.ok(!reMixed.test('startttend'));

  // Escaping pipe (alternation)
  const pipe = escapeStringRegexp('a|b');
  const rePipe = new RegExp('^' + pipe + '$');
  assert.ok(rePipe.test('a|b'));
  assert.ok(!rePipe.test('a'));
  assert.ok(!rePipe.test('b'));

  // Escaping backslash
  const backslash = escapeStringRegexp('path\\to\\file');
  const reBackslash = new RegExp('^' + backslash + '$');
  assert.ok(reBackslash.test('path\\to\\file'));

  // Escaping curly braces
  const braces = escapeStringRegexp('a{2,3}');
  const reBraces = new RegExp('^' + braces + '$');
  assert.ok(reBraces.test('a{2,3}'));
  assert.ok(!reBraces.test('aa'));
  assert.ok(!reBraces.test('aaa'));

  // Plain alphanumeric strings pass through unchanged
  assert.equal(escapeStringRegexp('abc123'), 'abc123');
  assert.equal(escapeStringRegexp('hello'), 'hello');

  // Use in a real search scenario: find literal string in text
  const searchTerm = 'price: $9.99 (USD)';
  const searchRe = new RegExp(escapeStringRegexp(searchTerm));
  assert.ok(searchRe.test('The price: $9.99 (USD) is final.'));
  assert.ok(!searchRe.test('The price: $9.98 (USD) is final.'));

  console.log('escape-string-regexp-test:ok');
})().catch((err) => {
  console.error('escape-string-regexp-test:fail');
  console.error(err && err.stack ? err.stack : String(err));
  process.exit(1);
});
