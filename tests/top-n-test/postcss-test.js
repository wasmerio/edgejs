'use strict';

const assert = require('node:assert/strict');
const postcss = require('postcss');

try {
  // Parse a CSS string
  const css = 'a { color: red; font-size: 14px; } .btn { display: flex; }';
  const root = postcss.parse(css);

  // Verify the parsed structure
  assert.equal(root.first.selector, 'a');
  assert.equal(root.nodes.length, 2);

  // Transform declarations: uppercase all color values
  root.walkDecls('color', (decl) => {
    decl.value = decl.value.toUpperCase();
  });
  const aRule = root.first;
  const colorDecl = aRule.nodes.find((n) => n.prop === 'color');
  assert.equal(colorDecl.value, 'RED');

  // Add a vendor prefix manually by cloning a declaration
  const displayDecl = root.nodes[1].nodes.find((n) => n.prop === 'display');
  const prefixed = displayDecl.clone({ prop: '-webkit-display' });
  root.nodes[1].insertBefore(displayDecl, prefixed);
  const btnProps = root.nodes[1].nodes.map((n) => n.prop);
  assert.ok(btnProps.includes('-webkit-display'), 'should have vendor-prefixed prop');
  assert.ok(btnProps.includes('display'), 'should still have original prop');

  // Stringify the result back to CSS
  const output = root.toString();
  assert.ok(output.includes('color: RED'), 'stringified CSS should have transformed value');
  assert.ok(output.includes('-webkit-display: flex'), 'stringified CSS should include prefix');

  // Use the plugin API: create a simple plugin that adds a comment
  const addComment = () => {
    return {
      postcssPlugin: 'add-comment',
      Once(root) {
        root.prepend(postcss.comment({ text: 'processed' }));
      },
    };
  };
  addComment.postcss = true;

  const result = postcss([addComment]).process('body { margin: 0; }', { from: undefined });
  assert.ok(result.css.includes('/* processed */'), 'plugin should add comment');
  assert.ok(result.css.includes('margin: 0'), 'original CSS should remain');

  console.log('postcss-test:ok');
} catch (err) {
  console.error('postcss-test:fail');
  console.error(err && err.stack ? err.stack : String(err));
  process.exit(1);
}
