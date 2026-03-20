'use strict';

const assert = require('node:assert/strict');

(async () => {
  const domhandler = await import('domhandler');
  const { Parser } = require('htmlparser2');

  const DomHandler = domhandler.DomHandler || domhandler.default;

  // Parse a simple HTML snippet and inspect the resulting DOM tree
  const dom = await new Promise((resolve, reject) => {
    const handler = new DomHandler((err, nodes) => {
      if (err) reject(err);
      else resolve(nodes);
    });
    const parser = new Parser(handler);
    parser.write('<div id="main"><p class="intro">Hello</p><span>World</span></div>');
    parser.end();
  });

  assert.ok(Array.isArray(dom), 'DOM should be an array of nodes');
  assert.ok(dom.length > 0, 'DOM should have at least one root node');

  // The root node should be the <div>
  const div = dom[0];
  assert.equal(div.type, 'tag', 'root node should be a tag');
  assert.equal(div.name, 'div', 'root tag should be a div');
  assert.equal(div.attribs.id, 'main', 'div should have id="main"');

  // Check children of the div
  const children = div.children.filter((c) => c.type === 'tag');
  assert.equal(children.length, 2, 'div should have two child tags');

  const p = children[0];
  assert.equal(p.name, 'p', 'first child should be a p');
  assert.equal(p.attribs.class, 'intro', 'p should have class "intro"');

  // Check text content inside p
  const pText = p.children.find((c) => c.type === 'text');
  assert.ok(pText, 'p should have a text child');
  assert.equal(pText.data, 'Hello', 'p text should be "Hello"');

  const span = children[1];
  assert.equal(span.name, 'span', 'second child should be a span');
  const spanText = span.children.find((c) => c.type === 'text');
  assert.equal(spanText.data, 'World', 'span text should be "World"');

  // Check parent references
  assert.equal(p.parent, div, 'p parent should be the div');
  assert.equal(span.parent, div, 'span parent should be the div');

  // Test with self-closing tags
  const dom2 = await new Promise((resolve, reject) => {
    const handler = new DomHandler((err, nodes) => {
      if (err) reject(err);
      else resolve(nodes);
    });
    const parser = new Parser(handler);
    parser.write('<br/><img src="test.png"/>');
    parser.end();
  });

  assert.ok(dom2.length >= 2, 'should parse self-closing tags');
  assert.equal(dom2[0].name, 'br');
  assert.equal(dom2[1].name, 'img');
  assert.equal(dom2[1].attribs.src, 'test.png');

  console.log('domhandler-test:ok');
})().catch((err) => {
  console.error('domhandler-test:fail');
  console.error(err && err.stack ? err.stack : String(err));
  process.exit(1);
});
