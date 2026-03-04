'use strict';

const REPL = require('repl');

module.exports = { __proto__: REPL };
module.exports.createInternalRepl = createRepl;

function createRepl(env, opts, cb) {
  if (typeof opts === 'function') {
    cb = opts;
    opts = null;
  }
  const safeEnv = env && typeof env === 'object' ? env : {};
  const options = {
    ignoreUndefined: false,
    useGlobal: true,
    breakEvalOnSigint: true,
    ...(opts && typeof opts === 'object' ? opts : {}),
  };

  if (Number.parseInt(safeEnv.NODE_NO_READLINE, 10)) {
    options.terminal = false;
  }

  if (typeof safeEnv.NODE_REPL_MODE === 'string') {
    const mode = safeEnv.NODE_REPL_MODE.toLowerCase().trim();
    options.replMode = mode === 'strict' ? REPL.REPL_MODE_STRICT :
      (mode === 'sloppy' ? REPL.REPL_MODE_SLOPPY : options.replMode);
  }

  if (options.replMode === undefined) {
    options.replMode = REPL.REPL_MODE_SLOPPY;
  }

  const historySize = Number(safeEnv.NODE_REPL_HISTORY_SIZE);
  options.size = Number.isNaN(historySize) || historySize <= 0 ? 1000 : historySize;
  const term = Object.prototype.hasOwnProperty.call(options, 'terminal') ?
    !!options.terminal : !!(process.stdout && process.stdout.isTTY);
  options.filePath = term ? String(safeEnv.NODE_REPL_HISTORY || '') : '';

  const repl = REPL.start(options);
  if (typeof repl.setupHistory === 'function') {
    const historyOptions = {
      historyPath: options.filePath,
      filePath: options.filePath,
      size: options.size,
      onHistoryFileLoaded: (err) => {
        if (typeof cb === 'function') cb(err || null, repl);
      },
    };
    if (repl.setupHistory.length >= 2) {
      repl.setupHistory(historyOptions, (err) => {
        if (typeof cb === 'function') cb(err || null, repl);
      });
    } else {
      repl.setupHistory(historyOptions);
    }
    return;
  }
  if (typeof cb === 'function') cb(null, repl);
}
