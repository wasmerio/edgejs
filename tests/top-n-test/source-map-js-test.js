'use strict';

const assert = require('node:assert/strict');

try {
  const { SourceMapGenerator, SourceMapConsumer } = require('source-map-js');

  // Create a source map generator for a compiled file
  const generator = new SourceMapGenerator({ file: 'output.js' });

  // Add mappings: generated position -> original position
  generator.addMapping({
    generated: { line: 1, column: 0 },
    original: { line: 1, column: 0 },
    source: 'input.js',
  });
  generator.addMapping({
    generated: { line: 2, column: 0 },
    original: { line: 3, column: 0 },
    source: 'input.js',
  });
  generator.addMapping({
    generated: { line: 3, column: 4 },
    original: { line: 5, column: 2 },
    source: 'input.js',
    name: 'myFunction',
  });

  // Set source content so consumers can look it up
  generator.setSourceContent('input.js', 'function myFunction() {\n  // line 2\n  console.log("hello");\n  // line 4\n  return true;\n}');

  // Convert to JSON
  const rawMap = generator.toJSON();
  assert.equal(rawMap.file, 'output.js');
  assert.equal(rawMap.version, 3);
  assert.ok(Array.isArray(rawMap.sources));
  assert.ok(rawMap.sources.includes('input.js'));
  assert.equal(typeof rawMap.mappings, 'string');
  assert.ok(rawMap.mappings.length > 0);

  // toString produces valid JSON
  const mapString = generator.toString();
  const parsed = JSON.parse(mapString);
  assert.equal(parsed.version, 3);
  assert.equal(parsed.file, 'output.js');

  // Parse with SourceMapConsumer and look up original positions
  const consumer = new SourceMapConsumer(rawMap);

  // Look up the original position for generated line 1, column 0
  const pos1 = consumer.originalPositionFor({ line: 1, column: 0 });
  assert.equal(pos1.source, 'input.js');
  assert.equal(pos1.line, 1);
  assert.equal(pos1.column, 0);

  // Look up the original position for generated line 3, column 4
  const pos3 = consumer.originalPositionFor({ line: 3, column: 4 });
  assert.equal(pos3.source, 'input.js');
  assert.equal(pos3.line, 5);
  assert.equal(pos3.column, 2);
  assert.equal(pos3.name, 'myFunction');

  // Look up the generated position from an original position
  const genPos = consumer.generatedPositionFor({
    source: 'input.js',
    line: 3,
    column: 0,
  });
  assert.equal(genPos.line, 2);
  assert.equal(genPos.column, 0);

  // sourceContentFor returns what we set
  const content = consumer.sourceContentFor('input.js');
  assert.ok(content.includes('myFunction'));

  // Iterate over all mappings
  const allMappings = [];
  consumer.eachMapping(function (mapping) {
    allMappings.push(mapping);
  });
  assert.equal(allMappings.length, 3);

  console.log('source-map-js-test:ok');
} catch (err) {
  console.error('source-map-js-test:fail');
  console.error(err && err.stack ? err.stack : String(err));
  process.exit(1);
}
