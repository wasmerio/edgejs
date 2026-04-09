'use strict';

const assert = require('node:assert/strict');

(async () => {
  const { ESLint } = require('eslint');

  // Create an ESLint instance with flat-style overrideConfig (v8+)
  const linter = new ESLint({
    useEslintrc: false,
    overrideConfig: {
      parserOptions: { ecmaVersion: 2022, sourceType: 'module' },
      rules: {
        semi: ['error', 'always'],
        'no-unused-vars': ['warn'],
      },
    },
  });

  // Lint code that is missing a semicolon - should produce an error
  const results = await linter.lintText('const x = 1\n');
  assert.equal(results.length, 1);

  const messages = results[0].messages;
  assert.ok(messages.length > 0, 'should have at least one message');

  // Find the semi rule violation
  const semiMsg = messages.find(m => m.ruleId === 'semi');
  assert.ok(semiMsg, 'should contain a semi rule violation');
  assert.equal(semiMsg.severity, 2); // 2 = error

  // Lint valid code - should produce no errors
  const cleanResults = await linter.lintText('const y = 2;\n');
  const cleanErrors = cleanResults[0].messages.filter(m => m.severity === 2);
  assert.equal(cleanErrors.length, 0, 'valid code should have no errors');

  // Lint code with multiple issues
  const multiResults = await linter.lintText('var a = 1\nvar b = 2\n');
  const multiMessages = multiResults[0].messages;
  const semiViolations = multiMessages.filter(m => m.ruleId === 'semi');
  assert.equal(semiViolations.length, 2, 'should find two missing semicolons');

  // The linter reports line numbers correctly
  assert.equal(semiViolations[0].line, 1);
  assert.equal(semiViolations[1].line, 2);

  // errorCount reflects the number of errors
  assert.ok(multiResults[0].errorCount >= 2);

  console.log('eslint-test:ok');
})().catch((err) => {
  console.error('eslint-test:fail');
  console.error(err && err.stack ? err.stack : String(err));
  process.exit(1);
});
