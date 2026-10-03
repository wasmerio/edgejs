'use strict'

require('./wasix-fs.cjs')

// WASIX subprocesses do not prepend a package command's main-args. Invoke the
// bundled npm CLI explicitly so pnpm's private fallback cannot treat "config"
// (or "publish") as an Edge script. Keep this adaptation local to pnpm.
if (process.platform === 'wasi') {
  const childProcess = require('child_process')
  const originalSpawnSync = childProcess.spawnSync
  childProcess.spawnSync = function spawnSyncWithNpmEntrypoint(file, args, options) {
    if (file === '/bin/edge-npm-internal') {
      if (!Array.isArray(args)) {
        options = args
        args = []
      }
      return originalSpawnSync.call(
        this,
        process.execPath,
        ['/npm/bin/npm-cli.js', ...args],
        options,
      )
    }
    return originalSpawnSync.apply(this, arguments)
  }
}

require('./pnpm.cjs')
