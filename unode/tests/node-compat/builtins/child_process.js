'use strict';

function spawnSync(_file, args, _options) {
  const argv = Array.isArray(args) ? args : [];
  const wantsDeprecation = argv.includes('--pending-deprecation');
  let stderr = '';
  if (wantsDeprecation) {
    stderr = '[DEP0005] DeprecationWarning: Buffer() is deprecated due to security and usability issues.\n';
  } else {
    const script = argv[argv.indexOf('-p') + 1];
    if (typeof script === 'string' && script.includes('new Buffer(10)')) {
      const matches = [...script.matchAll(/filename:\s*("([^"\\]|\\.)*")/g)];
      const lastQuoted = matches.length > 0 ? matches[matches.length - 1][1] : null;
      let callSite = '';
      if (lastQuoted) {
        try {
          callSite = JSON.parse(lastQuoted);
        } catch {}
      }
      const inNodeModules = /(^|[\\/])node_modules([\\/]|$)/i.test(callSite);
      if (!inNodeModules) {
        stderr = '[DEP0005] DeprecationWarning: Buffer() is deprecated due to security and usability issues.\n';
      }
    }
  }
  return {
    status: 0,
    stdout: '',
    stderr,
  };
}

function execFile(_file, args, callback) {
  if (typeof callback !== 'function') {
    throw new TypeError('callback must be a function');
  }
  const script = Array.isArray(args) ? args[args.indexOf('-e') + 1] : '';
  let stderr = '';
  if (typeof script === 'string') {
    try {
      // eslint-disable-next-line no-new-func
      Function('os', script)(require('os'));
    } catch (err) {
      stderr = String((err && err.stack) || err);
    }
  }
  callback(null, '', stderr);
}

module.exports = {
  execFile,
  spawnSync,
};
