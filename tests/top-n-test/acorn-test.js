'use strict';

const assert = require('node:assert/strict');

(async () => {
  const acorn = require('acorn');

  // Parse a simple variable declaration
  const ast1 = acorn.parse('const x = 42;', { ecmaVersion: 'latest', sourceType: 'module' });
  assert.equal(ast1.type, 'Program');
  assert.ok(Array.isArray(ast1.body));
  assert.equal(ast1.body.length, 1);
  assert.equal(ast1.body[0].type, 'VariableDeclaration');
  assert.equal(ast1.body[0].kind, 'const');
  assert.equal(ast1.body[0].declarations[0].id.name, 'x');
  assert.equal(ast1.body[0].declarations[0].init.value, 42);

  // Parse a function declaration
  const ast2 = acorn.parse('function greet(name) { return "hello " + name; }', { ecmaVersion: 'latest' });
  assert.equal(ast2.body[0].type, 'FunctionDeclaration');
  assert.equal(ast2.body[0].id.name, 'greet');
  assert.equal(ast2.body[0].params.length, 1);
  assert.equal(ast2.body[0].params[0].name, 'name');

  // Parse an arrow function expression
  const ast3 = acorn.parse('const add = (a, b) => a + b;', { ecmaVersion: 'latest' });
  const decl = ast3.body[0].declarations[0];
  assert.equal(decl.init.type, 'ArrowFunctionExpression');
  assert.equal(decl.init.params.length, 2);
  assert.equal(decl.init.params[0].name, 'a');
  assert.equal(decl.init.params[1].name, 'b');
  assert.equal(decl.init.body.type, 'BinaryExpression');
  assert.equal(decl.init.body.operator, '+');

  // Parse class declaration
  const ast4 = acorn.parse('class Foo extends Bar { constructor() { super(); } }', { ecmaVersion: 'latest' });
  assert.equal(ast4.body[0].type, 'ClassDeclaration');
  assert.equal(ast4.body[0].id.name, 'Foo');
  assert.equal(ast4.body[0].superClass.name, 'Bar');

  // Parse template literal
  const ast5 = acorn.parse('const s = `hello ${name}`;', { ecmaVersion: 'latest' });
  const tmpl = ast5.body[0].declarations[0].init;
  assert.equal(tmpl.type, 'TemplateLiteral');
  assert.ok(Array.isArray(tmpl.quasis));
  assert.ok(Array.isArray(tmpl.expressions));
  assert.equal(tmpl.expressions[0].name, 'name');

  // Invalid syntax should throw
  assert.throws(() => {
    acorn.parse('const = ;', { ecmaVersion: 'latest' });
  }, /Unexpected token/);

  // parseExpressionAt - parse an expression at a given offset
  if (typeof acorn.parseExpressionAt === 'function') {
    const expr = acorn.parseExpressionAt('void 1 + 2', 5, { ecmaVersion: 'latest' });
    assert.equal(expr.type, 'BinaryExpression');
  }

  // tokenizer
  if (typeof acorn.tokenizer === 'function') {
    const tokens = [...acorn.tokenizer('let x = 1;', { ecmaVersion: 'latest' })];
    assert.ok(tokens.length > 0);
    // Each token has a type and value
    for (const token of tokens) {
      assert.ok('type' in token);
      assert.ok('start' in token);
      assert.ok('end' in token);
    }
  }

  console.log('acorn-test:ok');
})().catch((err) => {
  console.error('acorn-test:fail');
  console.error(err && err.stack ? err.stack : String(err));
  process.exit(1);
});
