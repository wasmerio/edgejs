'use strict';

const assert = require('node:assert/strict');

const { Parser, parseDocument } = require('htmlparser2');

// Use handler callbacks to parse HTML
const tags = [];
const texts = [];
const closed = [];

const parser = new Parser({
  onopentag(name, attrs) {
    tags.push({ name, attrs });
  },
  ontext(text) {
    texts.push(text);
  },
  onclosetag(name) {
    closed.push(name);
  },
});

parser.write('<div class="main"><p>Hello</p><span>World</span></div>');
parser.end();

assert.equal(tags.length, 3);
assert.equal(tags[0].name, 'div');
assert.equal(tags[0].attrs.class, 'main');
assert.equal(tags[1].name, 'p');
assert.equal(tags[2].name, 'span');

assert.deepEqual(texts, ['Hello', 'World']);
assert.deepEqual(closed, ['p', 'span', 'div']);

// parseDocument returns a DOM tree
const doc = parseDocument('<ul><li>one</li><li>two</li></ul>');
assert.ok(doc);
assert.ok(doc.children.length > 0);

// The root element is <ul>
const ul = doc.children[0];
assert.equal(ul.name, 'ul');
assert.equal(ul.children.length, 2);
assert.equal(ul.children[0].name, 'li');
assert.equal(ul.children[1].name, 'li');

// Verify text content in li elements
assert.equal(ul.children[0].children[0].data, 'one');
assert.equal(ul.children[1].children[0].data, 'two');

console.log('htmlparser2-test:ok');
