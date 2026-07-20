'use strict';

// Ephemeral database provisioning for framework tests.
//
// Backend apps declare their database need in a top-level `database` block in
// routes.json:
//
//   {
//     "version": 1,
//     "database": {
//       "kind": "postgres",
//       "env": {
//         "CMD_DB_URL": "{dbUrl}",
//         "CMD_PORT": "{port}"
//       }
//     },
//     "routes": [...]
//   }
//
// The harness then starts a real database as a plain child process before the
// app's server starts (no Docker: framework tests also run on macOS CI
// runners, which have no Docker daemon) and injects the `env` block into the
// server environment. Values may reference:
//   {dbUrl} {dbHost} {dbPort} {dbUser} {dbPassword} {dbName}  — database info
//   {port}                                                     — the app port,
//     expanded later by makeProjectEnv once the harness picks it.
//
// An optional `setup` array lists shell commands (e.g. sequelize migrations
// and seeds) that the harness runs on the host toolchain after the database
// is up and before the app server starts, with the same env injected.
//
// Postgres is provided by the `embedded-postgres` npm package (real zonky.io
// binaries spawned via initdb/pg_ctl); MySQL by `mysql-memory-server` (uses a
// matching system mysqld when available, otherwise downloads official
// binaries). Both install on demand into <stateDir>/db-tools with the same
// pnpm store the project installs use.

const fs = require('node:fs');
const net = require('node:net');
const path = require('node:path');
const { spawnSync } = require('node:child_process');
const { createRequire } = require('node:module');

const EMBEDDED_POSTGRES_VERSION = '17.10.0-beta.17';
const MYSQL_MEMORY_SERVER_VERSION = '1.14.1';
// Semver range: a matching system mysqld is used as is (Linux CI images and
// most dev machines ship MySQL 8.0), otherwise the newest matching official
// binary is downloaded once and cached (the macOS CI case). Pinned to 8.0.x
// because the harness user authenticates with mysql_native_password (apps on
// the legacy `mysql` 2.x driver — e.g. Firekylin's think-mysql — cannot do
// caching_sha2_password), and 8.4+ disables that plugin by default.
const MYSQL_VERSION_RANGE = '8.0.x';
const DB_USER = 'framework';
const DB_PASSWORD = 'framework';
const DB_NAME = 'app';
const SUPPORTED_KINDS = ['postgres', 'mysql'];

const activeDatabases = new Set();
let signalHandlersInstalled = false;

function readProjectDatabaseConfig(project, routesJsonPath) {
  if (!fs.existsSync(routesJsonPath)) {
    return null;
  }

  let config = null;
  try {
    config = JSON.parse(fs.readFileSync(routesJsonPath, 'utf8'));
  } catch (error) {
    throw new Error(`invalid JSON in ${routesJsonPath}: ${error.message}`);
  }

  if (!config || typeof config !== 'object' || config.database == null) {
    return null;
  }

  const database = config.database;
  if (typeof database !== 'object') {
    throw new Error(`invalid database block in ${routesJsonPath}: expected an object`);
  }
  if (!SUPPORTED_KINDS.includes(database.kind)) {
    throw new Error(`invalid database block in ${routesJsonPath}: kind must be one of ${SUPPORTED_KINDS.join(', ')}`);
  }
  if (database.env != null && (typeof database.env !== 'object' || Array.isArray(database.env))) {
    throw new Error(`invalid database block in ${routesJsonPath}: env must be an object of string values`);
  }

  const env = {};
  for (const [name, value] of Object.entries(database.env || {})) {
    if (typeof value !== 'string') {
      throw new Error(`invalid database env value for ${name} in ${routesJsonPath}: expected a string`);
    }
    env[name] = value;
  }

  if (database.setup != null && !Array.isArray(database.setup)) {
    throw new Error(`invalid database block in ${routesJsonPath}: setup must be an array of shell commands`);
  }
  const setup = (database.setup || []).map((command) => {
    if (typeof command !== 'string' || !command.trim()) {
      throw new Error(`invalid database setup command in ${routesJsonPath}: expected a non-empty string`);
    }
    return command;
  });

  return { kind: database.kind, env, setup };
}

function ensureDatabaseTools(stateDir, pnpmStoreDir, log) {
  const toolsDir = path.join(stateDir, 'db-tools');
  const manifestPath = path.join(toolsDir, 'package.json');
  const manifest = {
    name: 'framework-test-db-tools',
    private: true,
    dependencies: {
      'embedded-postgres': EMBEDDED_POSTGRES_VERSION,
      'mysql-memory-server': MYSQL_MEMORY_SERVER_VERSION,
    },
  };
  const manifestJson = `${JSON.stringify(manifest, null, 2)}\n`;

  fs.mkdirSync(toolsDir, { recursive: true });
  const existing = fs.existsSync(manifestPath) ? fs.readFileSync(manifestPath, 'utf8') : null;
  if (existing !== manifestJson) {
    fs.writeFileSync(manifestPath, manifestJson);
  }

  let needsInstall = existing !== manifestJson;
  for (const [name, version] of Object.entries(manifest.dependencies)) {
    if (needsInstall) {
      break;
    }
    try {
      const installedMarker = path.join(toolsDir, 'node_modules', name, 'package.json');
      const installed = JSON.parse(fs.readFileSync(installedMarker, 'utf8'));
      needsInstall = installed.version !== version;
    } catch {
      needsInstall = true;
    }
  }

  if (needsInstall) {
    log(`installing database tools (${Object.entries(manifest.dependencies).map(([n, v]) => `${n} ${v}`).join(', ')}) into ${toolsDir}`);
    const args = ['install', '--no-lockfile', '--config.dangerouslyAllowAllBuilds=true'];
    if (pnpmStoreDir) {
      args.push('--store-dir', pnpmStoreDir);
    }
    const result = spawnSync('pnpm', args, { cwd: toolsDir, encoding: 'utf8' });
    if (result.status !== 0) {
      const detail = `${result.stdout || ''}${result.stderr || ''}`.trim();
      throw new Error(`pnpm install failed for database tools in ${toolsDir}${detail ? `:\n${detail}` : ''}`);
    }
  }

  return createRequire(manifestPath);
}

function allocateFreePort() {
  return new Promise((resolve, reject) => {
    const server = net.createServer();
    server.unref();
    server.on('error', reject);
    server.listen(0, '127.0.0.1', () => {
      const { port } = server.address();
      server.close((error) => (error ? reject(error) : resolve(port)));
    });
  });
}

function installSignalHandlers() {
  if (signalHandlersInstalled) {
    return;
  }
  signalHandlersInstalled = true;
  for (const signal of ['SIGINT', 'SIGTERM']) {
    process.on(signal, () => {
      stopAllDatabases().finally(() => {
        process.exit(signal === 'SIGINT' ? 130 : 143);
      });
    });
  }
}

async function startPostgres(requireTools, project, stage, stateDir) {
  const embeddedPostgres = requireTools('embedded-postgres');
  const EmbeddedPostgres = embeddedPostgres.default || embeddedPostgres;

  const port = await allocateFreePort();
  const dataDir = path.join(stateDir, 'db', `${project.name}.${stage.key}`);
  fs.rmSync(dataDir, { recursive: true, force: true });
  fs.mkdirSync(dataDir, { recursive: true });

  const instance = new EmbeddedPostgres({
    databaseDir: dataDir,
    user: DB_USER,
    password: DB_PASSWORD,
    port,
    persistent: false,
    onLog: () => {},
    onError: () => {},
  });

  await instance.initialise();
  await instance.start();
  await instance.createDatabase(DB_NAME);

  return {
    port,
    dataDir,
    urlScheme: 'postgres',
    stop: () => instance.stop(),
  };
}

async function startMysql(requireTools) {
  const { createDB } = requireTools('mysql-memory-server');

  // The package creates its `username` user without a password and only for
  // 'localhost'; apps connect over TCP with credentials, so create the
  // harness user with a password via the init SQL instead.
  const instance = await createDB({
    version: MYSQL_VERSION_RANGE,
    dbName: DB_NAME,
    logLevel: 'ERROR',
    downloadBinaryOnce: true,
    xEnabled: 'OFF',
    initSQLString: [
      `CREATE USER '${DB_USER}'@'%' IDENTIFIED WITH mysql_native_password BY '${DB_PASSWORD}';`,
      `GRANT ALL ON *.* TO '${DB_USER}'@'%' WITH GRANT OPTION;`,
    ].join('\n'),
  });

  return {
    port: instance.port,
    dataDir: null,
    urlScheme: 'mysql',
    stop: () => instance.stop(),
  };
}

const PROVIDERS = {
  postgres: startPostgres,
  mysql: startMysql,
};

async function startProjectDatabase(options) {
  const { config, project, stage, stateDir, pnpmStoreDir, log, logWarn } = options;
  const provider = PROVIDERS[config.kind];
  if (!provider) {
    throw new Error(`unsupported database kind: ${config.kind}`);
  }

  const requireTools = ensureDatabaseTools(stateDir, pnpmStoreDir, log);
  const instance = await provider(requireTools, project, stage, stateDir);

  const values = {
    dbHost: '127.0.0.1',
    dbPort: String(instance.port),
    dbUser: DB_USER,
    dbPassword: DB_PASSWORD,
    dbName: DB_NAME,
    dbUrl: `${instance.urlScheme}://${DB_USER}:${DB_PASSWORD}@127.0.0.1:${instance.port}/${DB_NAME}`,
  };

  const env = {};
  for (const [name, template] of Object.entries(config.env)) {
    env[name] = Object.entries(values).reduce(
      (value, [key, replacement]) => value.split(`{${key}}`).join(replacement),
      template,
    );
  }
  // The WASIX framework runner forwards only an allowlist of env vars into
  // the guest; it extends that allowlist with the names listed here.
  env.FRAMEWORK_TEST_EXTRA_ENV = Object.keys(env).join(',');

  const handle = {
    dataDir: instance.dataDir,
    env,
    kind: config.kind,
    port: instance.port,
    stopped: false,
    async stop() {
      if (handle.stopped) {
        return;
      }
      handle.stopped = true;
      activeDatabases.delete(handle);
      try {
        await instance.stop();
      } catch (error) {
        logWarn(`failed to stop ${config.kind} for ${project.name}: ${error.message}`);
      }
      if (instance.dataDir) {
        fs.rmSync(instance.dataDir, { recursive: true, force: true });
      }
    },
  };

  activeDatabases.add(handle);
  installSignalHandlers();
  return handle;
}

async function stopAllDatabases() {
  const handles = Array.from(activeDatabases);
  await Promise.allSettled(handles.map((handle) => handle.stop()));
}

module.exports = {
  readProjectDatabaseConfig,
  startProjectDatabase,
  stopAllDatabases,
};
