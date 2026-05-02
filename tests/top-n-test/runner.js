#!/usr/bin/env node
'use strict';

const fs = require('node:fs');
const path = require('node:path');
const cp = require('node:child_process');

const HERE = __dirname;
const RESULTS_DIR = path.join(HERE, 'results');
const DEFAULT_LOCK_FILE = path.join(HERE, 'packages.lock.json');
const VALID_RUNNERS = new Set(['node', 'edgejs', 'wasix_edgejs']);

function printUsage() {
  console.log([
    'Usage:',
    '  node runner.js -r <node|edgejs|wasix_edgejs> <package_name>',
    '  node runner.js -r <runner> --all',
    '  node runner.js --list',
    '',
    'Flags:',
    '  -r, --runner <name>     Runner mode (default: node)',
    '  --lock <path>           Locked package manifest (default: packages.lock.json)',
    '  --all                   Select all packages',
    '  --skip <a,b,...>        Skip package(s) by name',
    '  --list                  List packages from lock file',
    '  --no-fail-fast          Continue after failures (default is fail-fast)',
    '  -h, --help              Show help',
  ].join('\n'));
}

function parseSkipNames(rawValue) {
  return String(rawValue)
    .split(',')
    .map((entry) => entry.trim())
    .filter(Boolean);
}

function parseArgs(argv) {
  const opts = {
    runner: 'node',
    lockFile: DEFAULT_LOCK_FILE,
    all: false,
    list: false,
    failFast: true,
    requestedNames: [],
    skipNames: [],
  };

  for (let i = 0; i < argv.length; i += 1) {
    const arg = argv[i];
    if (arg === '-h' || arg === '--help') {
      opts.help = true;
      continue;
    }
    if (arg === '--all') {
      opts.all = true;
      continue;
    }
    if (arg === '--list') {
      opts.list = true;
      continue;
    }
    if (arg === '--no-fail-fast') {
      opts.failFast = false;
      continue;
    }
    if (arg === '--stop-on-fail') {
      opts.failFast = true;
      continue;
    }
    if (arg.startsWith('--skip=')) {
      opts.skipNames.push(...parseSkipNames(arg.slice('--skip='.length)));
      continue;
    }
    if (arg === '--skip') {
      const firstValue = argv[i + 1];
      if (!firstValue || firstValue.startsWith('-')) {
        throw new Error('--skip requires a package list, e.g. --skip ws,chalk');
      }

      const rawTokens = [firstValue];
      i += 1;
      while (
        i + 1 < argv.length
        && !argv[i + 1].startsWith('-')
        && /,\s*$/.test(rawTokens[rawTokens.length - 1])
      ) {
        rawTokens.push(argv[i + 1]);
        i += 1;
      }
      opts.skipNames.push(...parseSkipNames(rawTokens.join(' ')));
      continue;
    }

    if (arg.startsWith('--runner=')) {
      opts.runner = arg.slice('--runner='.length);
      continue;
    }
    if (arg === '--runner' || arg === '-r') {
      const value = argv[i + 1];
      if (!value) throw new Error(`${arg} requires a value`);
      opts.runner = value;
      i += 1;
      continue;
    }

    if (arg.startsWith('--lock=')) {
      opts.lockFile = path.resolve(HERE, arg.slice('--lock='.length));
      continue;
    }
    if (arg === '--lock') {
      const value = argv[i + 1];
      if (!value) throw new Error('--lock requires a value');
      opts.lockFile = path.resolve(HERE, value);
      i += 1;
      continue;
    }

    if (arg.startsWith('-')) {
      throw new Error(`Unknown flag: ${arg}`);
    }
    opts.requestedNames.push(arg);
  }

  return opts;
}

function runProcess(cmd, cmdArgs, cwd, extraEnv = {}) {
  const started = Date.now();
  const result = cp.spawnSync(cmd, cmdArgs, {
    cwd,
    encoding: 'utf8',
    stdio: ['ignore', 'pipe', 'pipe'],
    env: { ...process.env, ...extraEnv },
  });
  const stderrText = [result.stderr || '', result.error ? `spawn error: ${result.error.message}\n` : '']
    .filter(Boolean)
    .join('');
  return {
    ok: result.status === 0,
    status: result.status,
    signal: result.signal,
    ms: Date.now() - started,
    command: [cmd, ...cmdArgs].join(' '),
    stdout: result.stdout || '',
    stderr: stderrText,
  };
}

function loadLockFile(lockFile) {
  if (!fs.existsSync(lockFile)) {
    throw new Error(`Lock file not found: ${lockFile}`);
  }
  const parsed = JSON.parse(fs.readFileSync(lockFile, 'utf8'));
  const packages = Array.isArray(parsed.packages) ? parsed.packages : [];
  if (packages.length === 0) {
    throw new Error('No packages found in lock file.');
  }
  return {
    lock: parsed,
    packages,
    byName: new Map(packages.map((pkg) => [pkg.name, pkg])),
  };
}

function resolveRunnerCommand(runner, testFilePath) {
  if (runner === 'node') {
    return { cmd: 'node', args: [testFilePath], env: {} };
  }

  if (runner === 'edgejs') {
    const edgeBin = process.env.EDGEJS_BIN || 'edgejs';
    return { cmd: edgeBin, args: [testFilePath], env: {} };
  }

  if (runner === 'wasix_edgejs') {
    const wasmerBin = process.env.WASMER_BIN || 'wasmer-dev';
    const edgejsPkg = process.env.EDGEJS_PACKAGE || 'wasmer/edgejs';
    const registry = process.env.WASMER_REGISTRY || 'wasmer.io';
    const cwd = path.dirname(testFilePath);
    return {
      cmd: wasmerBin,
      args: ['run', edgejsPkg, '--registry', registry, '--net', `--volume=${cwd}`, '--', testFilePath],
      env: {},
    };
  }

  throw new Error(`Unsupported runner: ${runner}`);
}

function printFailureDetails(title, result) {
  console.log(`----- ${title} failure details -----`);
  console.log(`command: ${result.command}`);
  console.log(`exit_status: ${result.status} signal: ${result.signal || 'none'}`);
  console.log('[stdout]');
  process.stdout.write(result.stdout || '<empty>\n');
  if (result.stdout && !result.stdout.endsWith('\n')) process.stdout.write('\n');
  console.log('[stderr]');
  process.stdout.write(result.stderr || '<empty>\n');
  if (result.stderr && !result.stderr.endsWith('\n')) process.stdout.write('\n');
  console.log('----------------------------------');
}

function createFailureResult(command, stderr) {
  return {
    ok: false,
    status: null,
    signal: null,
    ms: 0,
    command,
    stdout: '',
    stderr,
  };
}

function logStepResult(label, result) {
  console.log(`${label} ... ${result.ok ? 'PASS' : 'FAIL'}`);
  if (!result.ok) {
    printFailureDetails(label, result);
  }
}

function main() {
  let opts;
  try {
    opts = parseArgs(process.argv.slice(2));
  } catch (err) {
    console.error(err.message);
    printUsage();
    process.exit(2);
  }

  if (opts.help) {
    printUsage();
    return;
  }

  if (!VALID_RUNNERS.has(opts.runner)) {
    console.error(`Invalid runner "${opts.runner}". Valid values: node, edgejs, wasix_edgejs`);
    process.exit(2);
  }

  let loaded;
  try {
    loaded = loadLockFile(opts.lockFile);
  } catch (err) {
    console.error(err.message);
    process.exit(1);
  }

  if (opts.list) {
    const lines = loaded.packages
      .sort((a, b) => (a.rank || Number.MAX_SAFE_INTEGER) - (b.rank || Number.MAX_SAFE_INTEGER))
      .map((pkg) => `${pkg.rank || '-'}\t${pkg.name}\tsmoke=${pkg.smokeTest || 'missing'}`);
    console.log(lines.join('\n'));
    return;
  }

  let selected;
  let skippedNames = [];
  try {
    selected = opts.all
      ? [...loaded.packages]
      : opts.requestedNames.map((name) => {
        const pkg = loaded.byName.get(name);
        if (!pkg) {
          throw new Error(`Package not found in lock file: ${name}`);
        }
        return pkg;
      });

    const uniqueSkipNames = [...new Set(opts.skipNames)];
    const unknownSkips = uniqueSkipNames.filter((name) => !loaded.byName.has(name));
    if (unknownSkips.length > 0) {
      throw new Error(`Unknown package(s) in --skip: ${unknownSkips.join(', ')}`);
    }
    if (uniqueSkipNames.length > 0) {
      const skipSet = new Set(uniqueSkipNames);
      skippedNames = selected.filter((pkg) => skipSet.has(pkg.name)).map((pkg) => pkg.name);
      selected = selected.filter((pkg) => !skipSet.has(pkg.name));
    }
  } catch (err) {
    console.error(err.message);
    process.exit(2);
  }

  if (selected.length === 0) {
    console.error('No packages selected. Use <package_name>, --all, or --list.');
    process.exit(2);
  }

  fs.mkdirSync(RESULTS_DIR, { recursive: true });

  const summary = {
    generatedAt: new Date().toISOString(),
    runner: opts.runner,
    lockFile: path.relative(HERE, opts.lockFile),
    failFast: opts.failFast,
    totals: {
      packages: selected.length,
      executed: 0,
      passed: 0,
      failed: 0,
      smokeFailed: 0,
    },
    packages: [],
  };

  let failedAny = false;

  for (const pkg of selected) {
    console.log(`Testing ${pkg.name}...`);

    const pkgSummary = {
      name: pkg.name,
      rank: pkg.rank,
      smokeTest: pkg.smokeTest || null,
      smoke: null,
      ok: false,
    };

    let packageOk = true;
    let smokeResult;
    if (!pkg.smokeTest) {
      packageOk = false;
      smokeResult = createFailureResult('<missing smoke test>', `Missing smokeTest for package ${pkg.name}`);
    } else {
      const absSmokePath = path.resolve(HERE, pkg.smokeTest);
      if (!fs.existsSync(absSmokePath)) {
        packageOk = false;
        smokeResult = createFailureResult(
          `<missing file ${pkg.smokeTest}>`,
          `Smoke test file does not exist: ${absSmokePath}`
        );
      } else {
        try {
          const spec = resolveRunnerCommand(opts.runner, absSmokePath);
          smokeResult = runProcess(spec.cmd, spec.args, HERE, spec.env);
        } catch (err) {
          packageOk = false;
          smokeResult = createFailureResult('<runner setup>', err.message);
        }
      }
    }

    pkgSummary.smoke = smokeResult;
    logStepResult(`smoke (${pkg.name})`, smokeResult);
    if (!smokeResult.ok) {
      packageOk = false;
      summary.totals.smokeFailed += 1;
    }

    pkgSummary.ok = packageOk;
    summary.packages.push(pkgSummary);
    summary.totals.executed += 1;

    if (packageOk) {
      summary.totals.passed += 1;
      console.log(`PASS ${pkg.name}`);
    } else {
      summary.totals.failed += 1;
      failedAny = true;
      console.log(`FAIL ${pkg.name}`);
      if (opts.failFast) break;
    }
  }

  const stamp = new Date().toISOString().replace(/[.:]/g, '-');
  const latest = path.join(RESULTS_DIR, `${opts.runner}-latest.json`);
  const stamped = path.join(RESULTS_DIR, `${opts.runner}-${stamp}.json`);
  fs.writeFileSync(latest, `${JSON.stringify(summary, null, 2)}\n`);
  fs.writeFileSync(stamped, `${JSON.stringify(summary, null, 2)}\n`);

  console.log(
    `\nSummary (${opts.runner}): ${summary.totals.passed}/${summary.totals.executed} passed (selected ${summary.totals.packages})`
  );

  const failedNames = summary.packages.filter((p) => !p.ok).map((p) => p.name);
  if (failedNames.length > 0) {
    console.log(`Failed (${failedNames.length}): ${failedNames.join(', ')}`);
  }
  if (skippedNames.length > 0) {
    console.log(`Skipped (${skippedNames.length}): ${skippedNames.join(', ')}`);
  }

  console.log(`Report: ${latest}`);

  process.exit(failedAny ? 1 : 0);
}

main();
