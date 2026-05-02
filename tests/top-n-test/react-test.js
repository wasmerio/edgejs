'use strict';

const assert = require('node:assert/strict');

const React = require('react');

// createElement with a simple HTML tag
const div = React.createElement('div', { id: 'root' }, 'Hello');
assert.equal(div.type, 'div', 'element type should be "div"');
assert.equal(div.props.id, 'root', 'should have id prop');
assert.equal(div.props.children, 'Hello', 'should have children');

// createElement with multiple children
const list = React.createElement('ul', null,
  React.createElement('li', { key: '1' }, 'one'),
  React.createElement('li', { key: '2' }, 'two')
);
assert.equal(list.type, 'ul');
assert.ok(Array.isArray(list.props.children), 'multiple children should be an array');
assert.equal(list.props.children.length, 2);
assert.equal(list.props.children[0].type, 'li');

// createElement with no props and no children
const br = React.createElement('br');
assert.equal(br.type, 'br');
assert.deepEqual(br.props, {}, 'should have empty props when none passed');

// Fragment
const frag = React.createElement(React.Fragment, null, 'a', 'b');
assert.equal(frag.type, React.Fragment, 'fragment type should be React.Fragment');
assert.ok(Array.isArray(frag.props.children));
assert.deepEqual(frag.props.children, ['a', 'b']);

// isValidElement
assert.equal(React.isValidElement(div), true, 'createElement result should be a valid element');
assert.equal(React.isValidElement('string'), false, 'string is not a valid element');
assert.equal(React.isValidElement(42), false, 'number is not a valid element');
assert.equal(React.isValidElement(null), false, 'null is not a valid element');
assert.equal(React.isValidElement({ type: 'div' }), false, 'plain object is not a valid element');

// createElement with a component function
function Greeting(props) {
  return React.createElement('span', null, 'Hi ' + props.name);
}
const greetEl = React.createElement(Greeting, { name: 'World' });
assert.equal(greetEl.type, Greeting, 'element type should be the component function');
assert.equal(greetEl.props.name, 'World');

// Check that key and ref concepts exist
const withKey = React.createElement('div', { key: 'mykey' });
assert.equal(withKey.key, 'mykey', 'element should have a key');

// Children utility
assert.equal(typeof React.Children, 'object', 'React.Children should exist');
assert.equal(typeof React.Children.map, 'function', 'React.Children.map should be a function');
assert.equal(typeof React.Children.forEach, 'function');
assert.equal(typeof React.Children.count, 'function');

const count = React.Children.count(list.props.children);
assert.equal(count, 2, 'Children.count should return 2 for two children');

console.log('react-test:ok');
