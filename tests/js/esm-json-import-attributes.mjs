// Regression test for wasmerio/edgejs#175: module requests must keep their
// `with { type: 'json' }` import attributes, or Node's ESM loader throws
// ERR_IMPORT_ATTRIBUTE_MISSING for every JSON module in the graph.
import assert from 'node:assert';
import { createRequire } from 'node:module';
import staticData from '../fixtures/import-attributes-data.json' with { type: 'json' };

assert.strictEqual(staticData.name, 'edgejs');
assert.strictEqual(staticData.answer, 42);

const dynamicNamespace = await import(
  '../fixtures/import-attributes-data.json',
  { with: { type: 'json' } }
);
assert.strictEqual(dynamicNamespace.default.answer, 42);

// require(esm) links synchronously (ModuleJob.syncLink), the path reported in
// the issue, and the dependency's own attributed JSON import must survive it.
const require = createRequire(import.meta.url);
const dep = require('../fixtures/import-attributes-dep.mjs');
assert.strictEqual(dep.name, 'edgejs');
assert.strictEqual(dep.default.answer, 42);

console.log('esm-json-import-attributes OK');
