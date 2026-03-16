// Intentionally plain JavaScript because the current local EdgeJS build does not
// have native TypeScript stripping enabled.

'use strict';

const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { spawn, spawnSync } = require('node:child_process');

const ROOT_DIR = path.resolve(__dirname, '..');
const EXAMPLES_DIR = path.join(ROOT_DIR, 'wasmer-examples');
const STATE_DIR = path.join(ROOT_DIR, '.framework-test');
const LOG_DIR = path.join(STATE_DIR, 'logs');
const PNPM_STORE_DIR = path.join(STATE_DIR, 'pnpm-store');
const DEFAULT_RUNNER = path.join(ROOT_DIR, 'build-edge', 'edge');
const SUBMODULE_HINT = 'git submodule update --init --recursive wasmer-examples';
const PNPM_HINT = 'Install pnpm and make sure it is on PATH. For example: corepack enable pnpm';

main().catch((error) => {
  const message = error && error.message ? error.message : String(error);
  process.stderr.write(`${formatPrefix('ERROR')} ${message}${os.EOL}`);
  process.exit(1);
});

async function main() {
  const args = process.argv.slice(2);
  const command = args[0];
  const selector = parseSelector(args.slice(1));

  if (command !== 'setup' && command !== 'reset') {
    fail([
      'usage: framework-test.js <setup|reset> [js-framework-name]',
      'example: framework-test.js setup js-next-staticsite',
    ].join('\n'));
  }

  if (command === 'reset') {
    reset(selector);
    return;
  }

  await setup(selector);
}

function parseSelector(args) {
  if (args.length > 1) {
    fail(`expected at most one framework selector, got: ${args.join(' ')}`);
  }

  if (args.length === 0) {
    return null;
  }

  const selector = args[0].replace(/\/+$/, '');
  return selector.startsWith('wasmer-examples/') ? path.basename(selector) : selector;
}

async function setup(selector) {
  const totalSteps = 6;
  logProgress(1, totalSteps, 'starting framework Phase 1 setup');
  ensureDir(LOG_DIR);
  ensureDir(PNPM_STORE_DIR);
  log('state directories ready');

  logProgress(2, totalSteps, 'checking prerequisites');
  ensurePnpm();
  log('pnpm is available on PATH');

  const projects = discoverProjects(selector);
  log(`discovered ${projects.length} framework${projects.length === 1 ? '' : 's'}`);

  logProgress(3, totalSteps, 'resolving runner target');
  const runner = resolveRunnerTarget();

  log(`using runner target: ${runner.targetPath}`);
  if (selector) {
    log(`selected framework: ${projects[0].name}`);
  } else {
    log(`selected frameworks (${projects.length}): ${projects.map((project) => project.name).join(', ')}`);
  }

  logProgress(4, totalSteps, `installing dependencies for ${projects.length} framework${projects.length === 1 ? '' : 's'}`);
  await installProjects(projects);

  logProgress(5, totalSteps, 'injecting runner symlinks');
  const injected = [];
  for (let index = 0; index < projects.length; index += 1) {
    const project = projects[index];
    log(`injecting runner for ${project.name} (${index + 1}/${projects.length})`);
    injected.push(injectRunner(project, runner.targetPath));
  }

  logProgress(6, totalSteps, 'phase 1 setup complete');
  log(`phase 1 setup complete for ${projects.length} framework${projects.length === 1 ? '' : 's'}`);
  for (const result of injected) {
    log(`prepared ${result.project.name} via ${result.compatibleLaunchers.join(', ')}`);
  }
  log('runtime validation is not implemented yet; this target currently performs setup only');
}

function ensurePnpm() {
  const check = spawnSync('pnpm', ['--version'], {
    cwd: ROOT_DIR,
    stdio: 'ignore',
  });

  if (check.error || check.status !== 0) {
    fail(`pnpm is required but was not found on PATH.\n${PNPM_HINT}`);
  }
}

function discoverProjects(selector) {
  if (!fs.existsSync(EXAMPLES_DIR)) {
    fail(`wasmer-examples is missing.\nRun: ${SUBMODULE_HINT}`);
  }

  const entries = fs.readdirSync(EXAMPLES_DIR, { withFileTypes: true });
  const projects = entries
    .filter((entry) => entry.isDirectory() && entry.name.startsWith('js-'))
    .map((entry) => ({
      name: entry.name,
      dir: path.join(EXAMPLES_DIR, entry.name),
      packageJson: path.join(EXAMPLES_DIR, entry.name, 'package.json'),
    }))
    .filter((project) => fs.existsSync(project.packageJson))
    .sort((left, right) => left.name.localeCompare(right.name));

  if (projects.length === 0) {
    fail(`wasmer-examples is not initialized or has no js-* packages.\nRun: ${SUBMODULE_HINT}`);
  }

  if (!selector) {
    return projects;
  }

  const match = projects.find((project) => project.name === selector);
  if (!match) {
    fail([
      `unknown framework selector: ${selector}`,
      `available frameworks: ${projects.map((project) => project.name).join(', ')}`,
    ].join('\n'));
  }

  return [match];
}

function resolveRunnerTarget() {
  const rawTarget = process.env.SYMLINK_TARGET && process.env.SYMLINK_TARGET.trim()
    ? process.env.SYMLINK_TARGET.trim()
    : DEFAULT_RUNNER;
  const targetPath = path.isAbsolute(rawTarget) ? rawTarget : path.resolve(ROOT_DIR, rawTarget);
  const defaultRunner = path.resolve(DEFAULT_RUNNER);
  const usingDefaultRunner = path.resolve(targetPath) === defaultRunner;

  if (usingDefaultRunner && !isExecutable(targetPath)) {
    log('default runner missing; building EdgeJS via make build');
    runSyncOrFail('make', ['build'], {
      cwd: ROOT_DIR,
      stdio: 'inherit',
    }, 'failed to build EdgeJS');
  }

  if (!isExecutable(targetPath)) {
    fail(`runner target is not executable: ${targetPath}`);
  }

  return { targetPath };
}

async function installProjects(projects) {
  log(`running pnpm install in parallel across ${projects.length} framework${projects.length === 1 ? '' : 's'}`);

  let completed = 0;
  const results = await Promise.all(projects.map(async (project, index) => {
    log(`pnpm install started for ${project.name} (${index + 1}/${projects.length})`);
    const result = await installProject(project);
    completed += 1;
    const status = result.ok ? 'completed' : 'failed';
    log(`pnpm install ${status} for ${project.name} (${completed}/${projects.length}, ${formatDuration(result.durationMs)})`);
    return result;
  }));
  const failures = results.filter((result) => !result.ok);

  if (failures.length > 0) {
    const lines = ['one or more pnpm install commands failed:'];
    for (const failure of failures) {
      lines.push(`- ${failure.project.name}: ${failure.logPath}`);
    }
    fail(lines.join('\n'));
  }
}

function installProject(project) {
  const logPath = path.join(LOG_DIR, `${project.name}.pnpm-install.log`);
  const startedAt = Date.now();

  return new Promise((resolve) => {
    const logStream = fs.createWriteStream(logPath, { flags: 'w' });
    let settled = false;
    const finish = (result) => {
      if (settled) {
        return;
      }
      settled = true;
      logStream.end(() => resolve({
        durationMs: Date.now() - startedAt,
        ...result,
      }));
    };

    logStream.write(`${formatPrefix('INFO')} pnpm install in ${project.name}${os.EOL}`);

    const child = spawn('pnpm', ['install', '--no-lockfile', '--store-dir', PNPM_STORE_DIR], {
      cwd: project.dir,
      env: process.env,
      stdio: ['ignore', 'pipe', 'pipe'],
    });

    child.stdout.on('data', (chunk) => {
      logStream.write(chunk);
    });
    child.stderr.on('data', (chunk) => {
      logStream.write(chunk);
    });

    child.on('error', (error) => {
      logStream.write(`${formatPrefix('ERROR')} spawn error: ${error.message}${os.EOL}`);
      finish({ ok: false, project, logPath });
    });

    child.on('close', (code, signal) => {
      if (signal) {
        logStream.write(`${formatPrefix('WARN')} signal: ${signal}${os.EOL}`);
      }
      logStream.write(`${formatPrefix(code === 0 ? 'INFO' : 'ERROR')} exit: ${code}${os.EOL}`);
      finish({ ok: code === 0, project, logPath });
    });
  });
}

function injectRunner(project, runnerTarget) {
  const binDir = path.join(project.dir, 'node_modules', '.bin');
  const compatibleLaunchers = findCompatibleLaunchers(binDir);

  if (compatibleLaunchers.length === 0) {
    fail(`no compatible pnpm launcher was found for ${project.name} in ${binDir}`);
  }

  const nodeShimPath = path.join(binDir, 'node');
  removeFileOrSymlink(nodeShimPath);
  fs.symlinkSync(runnerTarget, nodeShimPath);

  const resolvedShim = fs.realpathSync(nodeShimPath);
  const resolvedTarget = fs.realpathSync(runnerTarget);
  if (resolvedShim !== resolvedTarget) {
    fail(`runner shim for ${project.name} does not resolve to ${runnerTarget}`);
  }

  return {
    project,
    compatibleLaunchers,
  };
}

function findCompatibleLaunchers(binDir) {
  if (!fs.existsSync(binDir) || !fs.statSync(binDir).isDirectory()) {
    fail(`expected pnpm launcher directory to exist: ${binDir}`);
  }

  const compatible = [];
  const entries = fs.readdirSync(binDir, { withFileTypes: true });
  for (const entry of entries) {
    if (!entry.isFile() && !entry.isSymbolicLink()) {
      continue;
    }
    if (entry.name === 'node') {
      continue;
    }

    const launcherPath = path.join(binDir, entry.name);
    let content;
    try {
      content = fs.readFileSync(launcherPath, 'utf8');
    } catch (error) {
      continue;
    }

    if (content.includes('$basedir/node')) {
      compatible.push(entry.name);
    }
  }

  return compatible.sort();
}

function removeFileOrSymlink(targetPath) {
  if (!fs.existsSync(targetPath)) {
    return;
  }

  const stat = fs.lstatSync(targetPath);
  if (stat.isDirectory() && !stat.isSymbolicLink()) {
    fail(`refusing to remove directory at ${targetPath}`);
  }

  fs.rmSync(targetPath, { force: true });
}

function reset(selector) {
  const totalSteps = 3;
  logProgress(1, totalSteps, 'resetting framework-test generated state');

  const projects = safeDiscoverProjects(selector);
  log(`selected ${projects.length} framework${projects.length === 1 ? '' : 's'} for reset`);

  logProgress(2, totalSteps, 'removing generated framework state');
  for (const project of projects) {
    const nodeModulesPath = path.join(project.dir, 'node_modules');
    if (fs.existsSync(nodeModulesPath)) {
      log(`removing ${nodeModulesPath}`);
      fs.rmSync(nodeModulesPath, { recursive: true, force: true });
    }
  }

  if (fs.existsSync(STATE_DIR)) {
    log(`removing ${STATE_DIR}`);
    fs.rmSync(STATE_DIR, { recursive: true, force: true });
  }

  const buildDir = path.join(ROOT_DIR, 'build-edge');
  if (fs.existsSync(buildDir)) {
    log(`removing ${buildDir}`);
    fs.rmSync(buildDir, { recursive: true, force: true });
  }

  const rootStoreDir = path.join(ROOT_DIR, '.pnpm-store');
  if (fs.existsSync(rootStoreDir)) {
    log(`removing ${rootStoreDir}`);
    fs.rmSync(rootStoreDir, { recursive: true, force: true });
  }

  logProgress(3, totalSteps, 'framework-test reset complete');
  log('framework-test reset complete');
}

function safeDiscoverProjects(selector) {
  if (!fs.existsSync(EXAMPLES_DIR)) {
    return [];
  }

  try {
    return discoverProjects(selector);
  } catch (error) {
    if (selector) {
      throw error;
    }
    return [];
  }
}

function ensureDir(dirPath) {
  fs.mkdirSync(dirPath, { recursive: true });
}

function isExecutable(filePath) {
  try {
    fs.accessSync(filePath, fs.constants.X_OK);
    return true;
  } catch (error) {
    return false;
  }
}

function runSyncOrFail(command, args, options, errorMessage) {
  const result = spawnSync(command, args, options);
  if (result.error) {
    fail(`${errorMessage}: ${result.error.message}`);
  }
  if (result.status !== 0) {
    fail(`${errorMessage}: exit code ${result.status}`);
  }
}

function log(message) {
  process.stdout.write(`${formatPrefix('INFO')} ${message}${os.EOL}`);
}

function logProgress(current, total, message) {
  log(`[${current}/${total}] ${message}`);
}

function formatDuration(durationMs) {
  if (durationMs < 1000) {
    return `${durationMs}ms`;
  }

  const seconds = durationMs / 1000;
  if (seconds < 60) {
    return `${seconds.toFixed(1)}s`;
  }

  const minutes = Math.floor(seconds / 60);
  const remainingSeconds = Math.round(seconds % 60);
  return `${minutes}m ${remainingSeconds}s`;
}

function formatPrefix(level) {
  return `[${new Date().toISOString()}] [framework-test] [${level}]`;
}

function fail(message) {
  throw new Error(message);
}
