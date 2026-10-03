// Exercise the same release-please version bundled by the pinned GitHub action.
// Run with RELEASE_PLEASE_MODULES pointing to a temporary npm installation.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');

const modules = process.env.RELEASE_PLEASE_MODULES;
assert(modules, 'RELEASE_PLEASE_MODULES is required');
const root = path.resolve(__dirname, '../..');
const load = (name) => require(path.join(modules, name));
const read = (name) => fs.readFileSync(path.join(root, name), 'utf8');
const Ajv = load('ajv');
const config = JSON.parse(read('release-please-config.json'));
const schema = load('release-please/schemas/config.json');
const validate = new Ajv({ strict: false, validateFormats: false }).compile(schema);
assert(validate(config), JSON.stringify(validate.errors));
const { Version } = load('release-please/build/src/version');
const { Generic } = load('release-please/build/src/updaters/generic');
const { GenericToml } = load('release-please/build/src/updaters/generic-toml');
const { DefaultUpdater } = load('release-please/build/src/updaters/default');

const current = read('VERSION.txt').trim();
assert.equal(JSON.parse(read('.release-please-manifest.json'))['.'], current);
const currentParts = current.split('.');
['MAJOR', 'MINOR', 'PATCH'].forEach((name, index) => {
  const match = read('src/edge_version.h').match(new RegExp(`^#define EDGE_${name}_VERSION (\\d+)$`, 'm'));
  assert.equal(match?.[1], currentParts[index]);
});

// Change every component to test all C++ markers, including major/minor bumps.
const version = Version.parse('12.34.56');
assert.equal(new DefaultUpdater({ version }).updateContent(read('VERSION.txt')), '12.34.56\n');
for (const extra of config.packages['.']['extra-files']) {
  const before = read(extra.path);
  const updater = extra.type === 'toml'
    ? new GenericToml(extra.jsonpath, version)
    : new Generic({ version });
  const after = updater.updateContent(before);
  if (extra.type === 'toml') {
    assert.equal(after, before.replace(/^version = "[^"]+"/m, 'version = "12.34.56"'));
  } else {
    const expected = before
      .replace(/^#define EDGE_MAJOR_VERSION \d+$/m, '#define EDGE_MAJOR_VERSION 12')
      .replace(/^#define EDGE_MINOR_VERSION \d+$/m, '#define EDGE_MINOR_VERSION 34')
      .replace(/^#define EDGE_PATCH_VERSION \d+$/m, '#define EDGE_PATCH_VERSION 56');
    assert.equal(after, expected);
  }
  console.log(`Validated actual release-please updater: ${extra.path}`);
}
console.log('Validated release-please schema, version baseline, and extra-file updaters');
