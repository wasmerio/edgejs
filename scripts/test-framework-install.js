'use strict';

const assert = require('node:assert/strict');
const { execFileSync } = require('node:child_process');
const fs = require('node:fs');
const { createRequire } = require('node:module');
const os = require('node:os');
const path = require('node:path');
const test = require('node:test');
const { create } = require('./lib/framework-test-shared');

// Exercise the real package manager: a mocked argument check would miss flags
// which pnpm accepts but silently ignores. A local tarball needs no registry.
for (const legacyAllowlist of [false, true]) {
  test(`framework install runs dependency builds${legacyAllowlist ? ' with a legacy allowlist' : ''}`, async (t) => {
    const root = fs.mkdtempSync(path.join(os.tmpdir(), 'edgejs-framework-install-'));
    t.after(() => fs.rmSync(root, { recursive: true, force: true }));
    const dependency = path.join(root, 'package');
    fs.mkdirSync(dependency);
    fs.writeFileSync(path.join(dependency, 'package.json'), JSON.stringify({
      name: 'edgejs-build-fixture',
      version: '1.0.0',
      scripts: { postinstall: 'node build.js' },
    }));
    fs.writeFileSync(path.join(dependency, 'build.js'),
      "require('node:fs').writeFileSync(require('node:path').join(__dirname, 'built.txt'), 'built');\n");
    execFileSync('tar', ['-czf', path.join(root, 'dependency.tgz'), '-C', root, 'package']);

    const project = { name: 'js-fixture', dir: path.join(root, 'wasmer-examples', 'js-fixture') };
    fs.mkdirSync(project.dir, { recursive: true });
    fs.writeFileSync(path.join(project.dir, 'package.json'), JSON.stringify({
      name: project.name,
      private: true,
      scripts: { build: 'node check.cjs' },
      dependencies: { 'edgejs-build-fixture': 'file:../../dependency.tgz' },
      ...(legacyAllowlist ? { pnpm: { onlyBuiltDependencies: [
        'edgejs-build-fixture',
        'edgejs-build-fixture@file:../../dependency.tgz',
      ] } } : {}),
    }));
    fs.writeFileSync(path.join(project.dir, 'check.cjs'),
      "require('node:assert/strict').equal(require('node:fs').readFileSync(require.resolve('edgejs-build-fixture/built.txt'), 'utf8'), 'built');\n");

    const harness = create({ rootDir: root });
    harness.ensureDir(harness.LOG_DIR);
    harness.ensureDir(harness.PNPM_STORE_DIR);
    harness.ensurePnpm();
    await harness.installProjects([project]);
    const marker = path.join(project.dir, 'node_modules/edgejs-build-fixture/built.txt');
    assert.ok(fs.existsSync(marker), fs.readFileSync(path.join(harness.LOG_DIR, `${project.name}.pnpm-install.log`), 'utf8'));
    assert.equal(fs.readFileSync(marker, 'utf8'), 'built');

    // The explicit install is authoritative. Builds must not silently install
    // again with different settings (pnpm 12 defaults verifyDepsBeforeRun to
    // install), regenerate lockfiles, or require the source tarball again.
    fs.unlinkSync(path.join(root, 'dependency.tgz'));
    execFileSync('pnpm', ['run', 'build'], {
      cwd: project.dir,
      env: harness.pnpmScriptEnv(),
      stdio: 'pipe',
    });
    assert.ok(!fs.existsSync(path.join(project.dir, 'pnpm-lock.yaml')));
  });
}

for (const isGatsby of [false, true]) {
  test(`generated files can resolve transitive dependencies only for Gatsby (${isGatsby})`, async (t) => {
    const root = fs.mkdtempSync(path.join(os.tmpdir(), 'edgejs-framework-hoist-'));
    t.after(() => fs.rmSync(root, { recursive: true, force: true }));
    const pack = (name, dependencies = {}) => {
      const directory = path.join(root, name);
      const source = path.join(directory, 'package');
      fs.mkdirSync(source, { recursive: true });
      fs.writeFileSync(path.join(source, 'package.json'), JSON.stringify({
        name, version: '1.0.0', main: 'index.js', dependencies,
      }));
      fs.writeFileSync(path.join(source, 'index.js'), 'module.exports = 42;\n');
      const archive = path.join(directory, 'package.tgz');
      execFileSync('tar', ['-czf', archive, '-C', directory, 'package']);
      return `file:${archive}`;
    };
    const dependency = pack('edgejs-generated-dependency');
    const framework = isGatsby ? 'gatsby' : 'other-framework';
    const project = { name: 'js-fixture', dir: path.join(root, 'wasmer-examples', 'js-fixture') };
    fs.mkdirSync(project.dir, { recursive: true });
    fs.writeFileSync(path.join(project.dir, 'package.json'), JSON.stringify({
      name: project.name,
      private: true,
      dependencies: { [framework]: pack(framework, { 'edgejs-generated-dependency': dependency }) },
    }));

    const harness = create({ rootDir: root });
    harness.ensureDir(harness.LOG_DIR);
    harness.ensureDir(harness.PNPM_STORE_DIR);
    await harness.installProjects([project]);

    // Gatsby copies framework code into the app's .cache directory. Its
    // imports must resolve there, outside the framework's node_modules tree.
    const generatedRequire = createRequire(path.join(project.dir, '.cache', 'generated.js'));
    if (isGatsby) {
      assert.equal(generatedRequire('edgejs-generated-dependency'), 42);
    } else {
      assert.throws(() => generatedRequire('edgejs-generated-dependency'), { code: 'MODULE_NOT_FOUND' });
    }
  });
}
