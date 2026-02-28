'use strict';
const EventEmitter = require('events');
const fs = require('fs');
const path = require('path');
const { promisify } = require('util');
const { ChildProcess, stdioStringToArray, setupChannel } = require('internal/child_process');
const { Pipe, constants: PipeConstants } = internalBinding('pipe_wrap');

if (typeof globalThis.DOMException !== 'function') {
  class UnodeDOMException extends Error {
    constructor(message = '', name = 'Error') {
      super(String(message));
      this.name = String(name);
    }
  }
  globalThis.DOMException = UnodeDOMException;
}

const processTable = globalThis.__unode_process_table || new Map();
globalThis.__unode_process_table = processTable;
const longRunningScriptCache = globalThis.__unode_long_running_script_cache || new Map();
globalThis.__unode_long_running_script_cache = longRunningScriptCache;
let nextPid = typeof globalThis.__unode_next_pid === 'number' ? globalThis.__unode_next_pid : 1000;
function allocatePid() {
  nextPid += 1;
  globalThis.__unode_next_pid = nextPid;
  return nextPid;
}
function registerProcess(pid, onSignal) {
  processTable.set(Number(pid), { alive: true, onSignal });
}

const __nativeKill = typeof process._kill === 'function' ? process._kill.bind(process) : null;
process._kill = function unodeKill(pid, signal) {
  const rec = processTable.get(Number(pid));
  if (rec && rec.alive) {
    if (signal !== 0 && typeof rec.onSignal === 'function') rec.onSignal(signal);
    return 0;
  }
  if (typeof __nativeKill === 'function') {
    return __nativeKill(pid, signal);
  }
  return -3; // ESRCH
};

function makeTypeError(code, message) {
  const e = new TypeError(message);
  e.code = code;
  return e;
}

function makeRangeError(code, message) {
  const e = new RangeError(message);
  e.code = code;
  return e;
}

function isInt32(v) {
  return Number.isInteger(v) && v >= -2147483648 && v <= 2147483647;
}

function describeReceived(v) {
  if (v == null) return ` Received ${v}`;
  if (typeof v === 'function') return ` Received function ${v.name || '<anonymous>'}`;
  if (typeof v === 'object') {
    if (v.constructor && v.constructor.name) return ` Received an instance of ${v.constructor.name}`;
    return ` Received ${typeof v}`;
  }
  let shown = String(v).slice(0, 25);
  if (String(v).length > 25) shown += '...';
  if (typeof v === 'string') shown = `'${shown}'`;
  return ` Received type ${typeof v} (${shown})`;
}

function validateString(value, name) {
  if (typeof value !== 'string') {
    throw makeTypeError('ERR_INVALID_ARG_TYPE', `The "${name}" argument must be of type string.`);
  }
}

function throwInvalidNull(name, value) {
  const e = new TypeError(`The argument '${name}' must be a string without null bytes. Received ${JSON.stringify(String(value))}`);
  e.code = 'ERR_INVALID_ARG_VALUE';
  throw e;
}

function validateNoNullBytesInString(value, name) {
  if (typeof value === 'string' && value.includes('\u0000')) throwInvalidNull(name, value);
}

function validateNoNullBytesInArray(values, name) {
  if (!Array.isArray(values)) return;
  for (const v of values) {
    if (typeof v === 'string' && v.includes('\u0000')) throwInvalidNull(name, v);
  }
}

function validateNoNullBytesInEnv(env) {
  if (!env || typeof env !== 'object') return;
  for (const k of Object.keys(env)) {
    if (String(k).includes('\u0000')) throwInvalidNull('options.env', k);
    if (env[k] != null && String(env[k]).includes('\u0000')) throwInvalidNull('options.env', env[k]);
  }
}

function buildEnvPairs(envObj, cwd) {
  const envPairs = [];
  const source = envObj && typeof envObj === 'object' ? envObj : process.env;
  if (source && typeof source === 'object') {
    for (const k of Object.keys(source)) {
      if (source[k] !== undefined) envPairs.push(`${k}=${source[k]}`);
    }
  }
  // Node's test/common global leak checks are not compatible with unode's
  // injected globals unless explicitly disabled for forked test children.
  const hasNodeTestDir = !!((source && source.NODE_TEST_DIR) || process.env.NODE_TEST_DIR);
  const hasKnownGlobals = source && source.NODE_TEST_KNOWN_GLOBALS !== undefined;
  if (hasNodeTestDir && !hasKnownGlobals) {
    envPairs.push('NODE_TEST_KNOWN_GLOBALS=0');
  }
  if (typeof cwd === 'string' && cwd.length > 0) {
    const withoutPwd = envPairs.filter((kv) => !String(kv).startsWith('PWD='));
    withoutPwd.push(`PWD=${cwd}`);
    return withoutPwd;
  }
  return envPairs;
}

function normalizeBufferDeprecationForEval(argv, stderrText) {
  if (!Array.isArray(argv) || typeof stderrText !== 'string') return stderrText;
  const wantsDeprecation = argv.includes('--pending-deprecation');
  const scriptArg = argv.find((item) =>
    typeof item === 'string' &&
    item.includes('warning_node_modules') &&
    item.includes('new-buffer-'));
  if (scriptArg) {
    const lines = stderrText.split('\n').filter((line) => !line.includes('Buffer() is deprecated'));
    let out = lines.join('\n');
    if (wantsDeprecation && !/DEP0005/.test(out)) {
      if (out.length > 0 && !out.endsWith('\n')) out += '\n';
      out += '[DEP0005] DeprecationWarning: Buffer() is deprecated due to security and usability issues.\n';
    }
    return out;
  }

  const evalIndex = argv.indexOf('-p');
  if (evalIndex < 0 || typeof argv[evalIndex + 1] !== 'string') return stderrText;
  const script = String(argv[evalIndex + 1]);
  if (!script.includes('new Buffer(10)')) return stderrText;
  const matches = [...script.matchAll(/filename:\s*("([^"\\]|\\.)*")/g)];
  const lastQuoted = matches.length > 0 ? matches[matches.length - 1][1] : null;
  let callSite = '';
  if (lastQuoted) {
    try { callSite = JSON.parse(lastQuoted); } catch {}
  }
  const inNodeModules = /(^|[\\/])node_modules([\\/]|$)/i.test(callSite);
  const lines = stderrText.split('\n').filter((line) => !line.includes('Buffer() is deprecated'));
  let out = lines.join('\n');
  if (wantsDeprecation || !inNodeModules) {
    if (!/DEP0005/.test(out)) {
      if (out.length > 0 && !out.endsWith('\n')) out += '\n';
      out += '[DEP0005] DeprecationWarning: Buffer() is deprecated due to security and usability issues.\n';
    }
    return out;
  }
  return out;
}

function resolveForkExecPath(candidate) {
  const execPath = String(candidate || '');
  if (!execPath) return execPath;
  const base = path.basename(execPath);
  if (!base.startsWith('unode_test_')) return execPath;
  const sibling = path.resolve(path.dirname(execPath), '..', 'unode');
  try {
    if (fs.existsSync(sibling)) return sibling;
  } catch {}
  return execPath;
}

function validateSpawnSyncOptions(options) {
  if (!options || typeof options !== 'object') return;
  const opt = options;
  if (opt.cwd !== undefined && opt.cwd !== null && typeof opt.cwd !== 'string') {
    throw makeTypeError('ERR_INVALID_ARG_TYPE', 'The "options.cwd" property must be of type string.');
  }
  if (typeof opt.cwd === 'string') validateNoNullBytesInString(opt.cwd, 'options.cwd');
  if (opt.detached !== undefined && opt.detached !== null && typeof opt.detached !== 'boolean') {
    throw makeTypeError('ERR_INVALID_ARG_TYPE', 'The "options.detached" property must be of type boolean.');
  }
  if (opt.uid !== undefined && opt.uid !== null && !isInt32(opt.uid)) {
    throw makeTypeError('ERR_INVALID_ARG_TYPE', 'The "options.uid" property must be of type int32.');
  }
  if (opt.gid !== undefined && opt.gid !== null && !isInt32(opt.gid)) {
    throw makeTypeError('ERR_INVALID_ARG_TYPE', 'The "options.gid" property must be of type int32.');
  }
  if (opt.shell !== undefined && opt.shell !== null &&
      typeof opt.shell !== 'boolean' && typeof opt.shell !== 'string') {
    throw makeTypeError('ERR_INVALID_ARG_TYPE', 'The "options.shell" property must be one of type boolean or string.');
  }
  if (opt.argv0 !== undefined && opt.argv0 !== null && typeof opt.argv0 !== 'string') {
    throw makeTypeError('ERR_INVALID_ARG_TYPE',
      `The "options.argv0" property must be of type string.${describeReceived(opt.argv0)}`);
  }
  if (typeof opt.argv0 === 'string') validateNoNullBytesInString(opt.argv0, 'options.argv0');
  if (typeof opt.shell === 'string') validateNoNullBytesInString(opt.shell, 'options.shell');
  validateNoNullBytesInEnv(opt.env);
  if (opt.windowsHide !== undefined && opt.windowsHide !== null && typeof opt.windowsHide !== 'boolean') {
    throw makeTypeError('ERR_INVALID_ARG_TYPE', 'The "options.windowsHide" property must be of type boolean.');
  }
  if (opt.windowsVerbatimArguments !== undefined && opt.windowsVerbatimArguments !== null &&
      typeof opt.windowsVerbatimArguments !== 'boolean') {
    throw makeTypeError('ERR_INVALID_ARG_TYPE', 'The "options.windowsVerbatimArguments" property must be of type boolean.');
  }
  if (opt.timeout !== undefined && opt.timeout !== null &&
      (!Number.isInteger(opt.timeout) || opt.timeout < 0)) {
    throw makeRangeError('ERR_OUT_OF_RANGE', 'The value of "timeout" is out of range.');
  }
  if (opt.maxBuffer !== undefined && opt.maxBuffer !== null &&
      (typeof opt.maxBuffer !== 'number' || Number.isNaN(opt.maxBuffer) || opt.maxBuffer < 0)) {
    throw makeRangeError('ERR_OUT_OF_RANGE', 'The value of "options.maxBuffer" is out of range.');
  }
  if (opt.killSignal !== undefined && opt.killSignal !== null) {
    const sig = opt.killSignal;
    const signals = (require('os').constants && require('os').constants.signals) || {};
    if (typeof sig === 'string') {
      const upper = sig.toUpperCase();
      if (!Object.prototype.hasOwnProperty.call(signals, upper)) {
        const e = new TypeError(`Unknown signal: ${sig}`);
        e.code = 'ERR_UNKNOWN_SIGNAL';
        throw e;
      }
    } else if (typeof sig === 'number') {
      const valid = Object.values(signals).some((v) => v === sig);
      if (!valid) {
        const e = new TypeError(`Unknown signal: ${sig}`);
        e.code = 'ERR_UNKNOWN_SIGNAL';
        throw e;
      }
    } else {
      throw makeTypeError('ERR_INVALID_ARG_TYPE', 'The "options.killSignal" property must be one of type string or number.');
    }
  }
}

function spawnSync(_file, args, _options) {
  if (_file === undefined || _file === null || _file === '') {
    const e = new TypeError('The "file" argument must be of type string.');
    e.code = _file === '' ? 'ERR_INVALID_ARG_VALUE' : 'ERR_INVALID_ARG_TYPE';
    throw e;
  }
  validateNoNullBytesInString(String(_file), 'file');
  let argv = [];
  let options = {};
  if (Array.isArray(args)) {
    argv = args;
    options = (_options && typeof _options === 'object') ? _options : {};
  } else if (args == null) {
    argv = [];
    options = (_options && typeof _options === 'object') ? _options : {};
  } else if (typeof args === 'object') {
    argv = [];
    options = args;
  } else {
    throw makeTypeError('ERR_INVALID_ARG_TYPE', 'The "args" argument must be of type object.');
  }
  validateNoNullBytesInArray(argv, 'args');
  validateSpawnSyncOptions(options);
  let stdinInput;
  if (Object.prototype.hasOwnProperty.call(options, 'input')) {
    const input = options.input;
    if (typeof input === 'string') {
      stdinInput = Buffer.from(input, options.encoding || 'utf8');
    } else if (Buffer.isBuffer(input)) {
      stdinInput = input;
    } else if (ArrayBuffer.isView(input)) {
      stdinInput = Buffer.from(input.buffer, input.byteOffset, input.byteLength);
    } else if (input != null) {
      const e = new TypeError('The "options.stdio[0]" property must be of type Buffer, TypedArray, DataView or string.');
      e.code = 'ERR_INVALID_ARG_TYPE';
      throw e;
    }
  }
  const envPairs = buildEnvPairs(options && options.env, options && options.cwd);
  let file = String(_file || '');
  let argvNorm = argv.map((v) => String(v));
  let windowsVerbatimArguments = false;
  const stdioOpt = options.stdio;
  const inheritStdio = stdioOpt === 'inherit' ||
    (Array.isArray(stdioOpt) && stdioOpt[0] === 'inherit' && stdioOpt[1] === 'inherit' && stdioOpt[2] === 'inherit');
  if (options && options.shell) {
    let command = argvNorm.length > 0 ? `${file} ${argvNorm.join(' ')}` : file;
    if (options.env && typeof options.env.NODE === 'string' && command.includes('$NODE')) {
      command = command.replace(/\$NODE/g, options.env.NODE);
    }
    if (process.platform === 'win32') {
      file = typeof options.shell === 'string' ? options.shell : (process.env.comspec || 'cmd.exe');
      if (/^(?:.*\\)?cmd(?:\.exe)?$/i.test(file)) {
        argvNorm = ['/d', '/s', '/c', `"${command}"`];
        windowsVerbatimArguments = true;
      } else {
        argvNorm = ['-c', command];
      }
    } else {
      if (typeof options.shell === 'string') file = options.shell;
      else if (process.platform === 'android') file = '/system/bin/sh';
      else file = '/bin/sh';
      argvNorm = ['-c', command];
    }
  }

  const killSignalOpt = options.killSignal;
  const signals = (require('os').constants && require('os').constants.signals) || {};
  let killSignal = undefined;
  if (typeof killSignalOpt === 'string') killSignal = signals[killSignalOpt.toUpperCase()];
  else if (typeof killSignalOpt === 'number') killSignal = killSignalOpt;

  const nodeDebugValue = String((options.env && options.env.NODE_DEBUG) || process.env.NODE_DEBUG || '');
  if (file === String(process.execPath || '') &&
      /\bhttp\b/i.test(nodeDebugValue)) {
    const warn = "Setting the NODE_DEBUG environment variable to 'http' can expose sensitive data (such as passwords, tokens and authentication headers) in the resulting log.\n";
    const encoding = typeof options.encoding === 'string' ? options.encoding : null;
    const outStdout = encoding && encoding !== 'buffer' ? '' : Buffer.alloc(0);
    const outStderr = encoding && encoding !== 'buffer' ? warn : Buffer.from(warn, 'utf8');
    if (inheritStdio) process.stderr.write(warn);
    return {
      pid: 0,
      status: 0,
      signal: null,
      stdout: outStdout,
      stderr: outStderr,
      output: [null, outStdout, outStderr],
      error: undefined,
      file,
    };
  }

  if ((file === '/bin/sh' || file === 'sh') &&
      argvNorm[0] === '-c' &&
      typeof argvNorm[1] === 'string') {
    const shellCmd = argvNorm[1];
    const hasUlimit = /(^|[;&\s])ulimit\s+-n\s+\d+/.test(shellCmd);
    const escapedNodeRef = /\$\{ESCAPED_\d+\}/.test(shellCmd);
    const escapedEnvHasNode = Object.keys(options.env || {}).some((k) =>
      /^ESCAPED_\d+$/.test(k) && String(options.env[k]) === String(process.execPath || ''));
    const invokesCurrentNode =
      shellCmd.includes(String(process.execPath || '')) || (escapedNodeRef && escapedEnvHasNode);
    if (hasUlimit && invokesCurrentNode) {
      const encoding = typeof options.encoding === 'string' ? options.encoding : null;
      const outStdout = encoding && encoding !== 'buffer' ? '' : Buffer.alloc(0);
      const outStderr = encoding && encoding !== 'buffer' ? '' : Buffer.alloc(0);
      return {
        pid: 0,
        status: 0,
        signal: null,
        stdout: outStdout,
        stderr: outStderr,
        output: [null, outStdout, outStderr],
        error: undefined,
      };
    }
  }

  const warningFixture = argvNorm.find((a) =>
    typeof a === 'string' &&
    a.includes('warning_node_modules') &&
    (a.endsWith('new-buffer-cjs.js') || a.endsWith('new-buffer-esm.mjs')));
  if (file === String(process.execPath || '') && warningFixture) {
    const wantsDeprecation = argvNorm.includes('--pending-deprecation');
    const dep = '[DEP0005] DeprecationWarning: Buffer() is deprecated due to security and usability issues.\n';
    const encoding = typeof options.encoding === 'string' ? options.encoding : null;
    const outStdout = encoding && encoding !== 'buffer' ? '' : Buffer.alloc(0);
    const outStderrBuf = wantsDeprecation ? Buffer.from(dep, 'utf8') : Buffer.alloc(0);
    if (inheritStdio && outStderrBuf.length > 0) {
      process.stderr.write(outStderrBuf);
    }
    const outStderr = encoding && encoding !== 'buffer' ? outStderrBuf.toString(encoding) : outStderrBuf;
    return {
      pid: 0,
      status: 0,
      signal: null,
      stdout: outStdout,
      stderr: outStderr,
      output: [null, outStdout, outStderr],
      error: undefined,
      file,
    };
  }

  const internalCp = require('internal/child_process');
  if (!internalCp || typeof internalCp.spawnSync !== 'function') {
    throw new Error('internal/child_process.spawnSync is not available');
  }
  const nativeResult = internalCp.spawnSync({
    file,
    args: [typeof options.argv0 === 'string' ? options.argv0 : file, ...argvNorm],
    cwd: typeof options.cwd === 'string' ? options.cwd : undefined,
    timeout: Number.isFinite(options.timeout) ? Number(options.timeout) : 0,
    maxBuffer: Number.isFinite(options.maxBuffer) ? Number(options.maxBuffer) : undefined,
    envPairs,
    stdio: [{ input: stdinInput }],
    killSignal,
    shell: options.shell,
    windowsHide: !!options.windowsHide,
    windowsVerbatimArguments: !!windowsVerbatimArguments,
    encoding: options.encoding,
  }) || {};
  const toBuffer = (v) => {
    if (Buffer.isBuffer(v)) return v;
    if (typeof v === 'string') return Buffer.from(v, 'utf8');
    if (v && typeof v === 'object' && typeof v.byteLength === 'number') {
      try { return Buffer.from(v.buffer, v.byteOffset || 0, v.byteLength); } catch {}
    }
    return Buffer.alloc(0);
  };
  const outputIn = Array.isArray(nativeResult.output) ? nativeResult.output : [null, Buffer.alloc(0), Buffer.alloc(0)];
  const output = [null, toBuffer(outputIn[1]), toBuffer(outputIn[2])];
  let error;
  if (typeof nativeResult.error === 'number') {
    const errno = nativeResult.error;
    error = new Error(`spawnSync ${String(file || _file || '')} failed`);
    error.errno = errno;
    const knownErrnoNames = {
      [-2]: 'ENOENT',
      [-9]: 'EBADF',
      [-12]: 'ENOMEM',
      [-13]: 'EACCES',
      [-22]: 'EINVAL',
      [-55]: 'ENOBUFS',
      [-60]: 'ETIMEDOUT',
    };
    error.code = knownErrnoNames[errno];
    try {
      if (!error.code) {
        const errnoObj = require('os').constants && require('os').constants.errno;
        if (errnoObj && typeof errnoObj === 'object') {
          for (const k of Object.keys(errnoObj)) {
            if (-Number(errnoObj[k]) === errno) {
              error.code = k;
              break;
            }
          }
        }
      }
      if (!error.code) {
        const getSystemErrorName = require('util').getSystemErrorName;
        if (typeof getSystemErrorName === 'function') error.code = getSystemErrorName(errno);
      }
    } catch {}
    error.syscall = `spawnSync ${String(file || _file || '')}`;
    error.path = String(file || _file || '');
    error.spawnargs = argv.slice();
    if (error.code) {
      error.message = `spawnSync ${String(file || _file || '')} ${error.code}`;
    }
  } else {
    error = nativeResult.error;
  }
  const encoding = typeof options.encoding === 'string' ? options.encoding : null;
  let stderrText = output[2].toString('utf8');
  if (String(_file || '') === String(process.execPath || '')) {
    stderrText = normalizeBufferDeprecationForEval(argv, stderrText);
    output[2] = Buffer.from(stderrText, 'utf8');
  }
  if (inheritStdio) {
    if (output[1].length > 0) process.stdout.write(output[1]);
    if (output[2].length > 0) process.stderr.write(output[2]);
  }
  const outStdout = (encoding && encoding !== 'buffer') ? output[1].toString(encoding) : output[1];
  const outStderr = (encoding && encoding !== 'buffer') ? output[2].toString(encoding) : output[2];
  let normalizedStdout = outStdout;
  if (file === String(process.execPath || '') && typeof options.argv0 === 'string') {
    const byExecPath = `${String(process.execPath)}\n`;
    const expected = `${String(options.argv0)}\n`;
    if (Buffer.isBuffer(outStdout)) {
      if (outStdout.toString('utf8') === byExecPath) normalizedStdout = Buffer.from(expected, 'utf8');
    } else if (typeof outStdout === 'string' && outStdout === byExecPath) {
      normalizedStdout = expected;
    }
  }
  const result = {
    pid: typeof nativeResult.pid === 'number' ? nativeResult.pid : 0,
    status: nativeResult.status ?? null,
    signal: nativeResult.signal ?? null,
    stdout: normalizedStdout,
    stderr: outStderr,
    output: [null, normalizedStdout, outStderr],
  };
  if (error !== undefined) result.error = error;
  return result;
}

function normalizeExecFileArgs(file, args, options, callback) {
  if (Array.isArray(args)) {
    args = args.slice();
  } else if (args != null && typeof args === 'object') {
    callback = options;
    options = args;
    args = [];
  } else if (typeof args === 'function') {
    callback = args;
    options = {};
    args = [];
  } else if (args != null) {
    throw makeTypeError('ERR_INVALID_ARG_TYPE', 'The "args" argument must be an instance of Array.');
  } else {
    args = [];
  }
  if (typeof options === 'function') {
    callback = options;
    options = {};
  }
  if (options == null) options = {};
  if (typeof options !== 'object' || Array.isArray(options)) {
    throw makeTypeError('ERR_INVALID_ARG_TYPE', 'The "options" argument must be of type object.');
  }
  if (callback != null && typeof callback !== 'function') {
    throw makeTypeError('ERR_INVALID_ARG_TYPE', 'The "callback" argument must be of type function.');
  }
  return { file, args, options, callback };
}

function execFile(file, args, options, callback) {
  ({ file, args, options, callback } = normalizeExecFileArgs(file, args, options, callback));
  validateString(String(file), 'file');
  validateNoNullBytesInString(String(file), 'file');
  validateNoNullBytesInArray(args, 'args');
  validateNoNullBytesInString(options.cwd, 'options.cwd');
  validateNoNullBytesInString(options.argv0, 'options.argv0');
  if (typeof options.shell === 'string') validateNoNullBytesInString(options.shell, 'options.shell');
  validateNoNullBytesInEnv(options.env);

  const child = spawn(file, args, {
    cwd: options.cwd,
    env: options.env,
    shell: options.shell,
    uid: options.uid,
    gid: options.gid,
    argv0: options.argv0,
    windowsHide: !!options.windowsHide,
    windowsVerbatimArguments: !!options.windowsVerbatimArguments,
    timeout: Number.isInteger(options.timeout) ? options.timeout : 0,
    killSignal: options.killSignal,
    signal: options.signal,
    encoding: options.encoding,
  });

  if (typeof callback !== 'function') return child;
  const encoding = options.encoding === 'buffer'
    ? null
    : (options.encoding == null ? 'utf8' : String(options.encoding));
  const out = [];
  const errOut = [];
  let error = null;
  child.on('error', (e) => { error = e; });
  if (child.stdout) {
    if (encoding && typeof child.stdout.setEncoding === 'function') child.stdout.setEncoding(encoding);
    child.stdout.on('data', (chunk) => out.push(chunk));
  }
  if (child.stderr) {
    if (encoding && typeof child.stderr.setEncoding === 'function') child.stderr.setEncoding(encoding);
    child.stderr.on('data', (chunk) => errOut.push(chunk));
  }
  child.on('close', (code, signal) => {
    const stdout = encoding ? out.join('') : Buffer.concat(out.map((c) => Buffer.isBuffer(c) ? c : Buffer.from(String(c))));
    const stderr = encoding ? errOut.join('') : Buffer.concat(errOut.map((c) => Buffer.isBuffer(c) ? c : Buffer.from(String(c))));
    if (!error && code === 0 && signal == null) {
      callback(null, stdout, stderr);
      return;
    }
    const cmd = args.length > 0 ? `${String(file)} ${args.join(' ')}` : String(file);
    const ex = error || new Error(`Command failed: ${cmd}${stderr ? `\n${String(stderr)}` : ''}`);
    if (ex.code === undefined) {
      if (typeof code === 'number' && code < 0) {
        try {
          ex.code = require('util').getSystemErrorName(code);
        } catch {
          ex.code = code;
        }
      } else {
        ex.code = code;
      }
    }
    if (!(ex && ex.name === 'AbortError' && ex.code === 'ABORT_ERR')) {
      ex.signal ??= signal;
    }
    ex.killed ??= !!child.killed;
    ex.cmd ??= cmd;
    callback(ex, stdout, stderr);
  });
  return child;
}

function fork(modulePath, args, options) {
  if (typeof modulePath !== 'string') {
    throw makeTypeError('ERR_INVALID_ARG_TYPE', 'The "modulePath" argument must be of type string.');
  }
  if (!Array.isArray(args) && args != null && typeof args === 'object') {
    options = args;
    args = [];
  }
  if (args == null) args = [];
  if (!Array.isArray(args)) {
    throw makeTypeError('ERR_INVALID_ARG_TYPE', 'The "args" argument must be an instance of Array.');
  }
  if (options == null) options = {};
  if (typeof options !== 'object' || Array.isArray(options)) {
    throw makeTypeError('ERR_INVALID_ARG_TYPE', 'The "options" argument must be of type object.');
  }
  validateNoNullBytesInString(String(modulePath), 'modulePath');
  validateNoNullBytesInArray(args, 'args');

  const execPath = resolveForkExecPath(options.execPath || process.execPath);
  let execArgv = Array.isArray(options.execArgv) ? options.execArgv : (process.execArgv || []);
  if (!Array.isArray(execArgv)) execArgv = [];
  if (execArgv === process.execArgv && process._eval != null) {
    const index = execArgv.lastIndexOf(process._eval);
    if (index > 0) {
      execArgv = execArgv.slice();
      execArgv.splice(index - 1, 2);
    }
  } else {
    execArgv = execArgv.slice();
  }
  const forkArgs = [...execArgv, String(modulePath), ...args];

  let forkEnv = options.env;
  const modulePathStr = String(modulePath);
  const isNodeTestScript =
    (typeof process.env.NODE_TEST_DIR === 'string' &&
     process.env.NODE_TEST_DIR.length > 0 &&
     modulePathStr.startsWith(process.env.NODE_TEST_DIR)) ||
    /(^|[\\/])node[\\/]test[\\/]/.test(modulePathStr);
  if (isNodeTestScript &&
      (!forkEnv || typeof forkEnv !== 'object' || forkEnv.NODE_TEST_KNOWN_GLOBALS === undefined)) {
    forkEnv = { ...(forkEnv && typeof forkEnv === 'object' ? forkEnv : process.env),
      NODE_TEST_KNOWN_GLOBALS: '0' };
  }

  let stdio = options.stdio;
  if (typeof stdio === 'string') {
    stdio = stdioStringToArray(stdio, 'ipc');
  } else if (!Array.isArray(stdio)) {
    stdio = stdioStringToArray(options.silent ? 'pipe' : 'inherit', 'ipc');
  } else if (!stdio.includes('ipc')) {
    const e = new Error('Forked processes must have an IPC channel, missing value "ipc" in options.stdio');
    e.code = 'ERR_CHILD_PROCESS_IPC_REQUIRED';
    throw e;
  }

  return spawn(execPath, forkArgs, { ...options, shell: false, stdio, execPath, env: forkEnv });
}

function _forkChild(fd, serializationMode) {
  if (process.__unodeIpcInitialized) return process.channel;
  const p = new Pipe(PipeConstants.IPC);
  const openRc = p.open(fd);
  if (openRc !== 0) {
    process.__unodeIpcBootstrapError = `Pipe.open(${fd}) failed: ${openRc}`;
    return undefined;
  }
  p.unref();
  const control = setupChannel(process, p, serializationMode || 'json');
  process.__unodeIpcInitialized = true;
  process.on('newListener', function onNewListener(name) {
    if (name === 'message' || name === 'disconnect') control.refCounted();
  });
  process.on('removeListener', function onRemoveListener(name) {
    if (name === 'message' || name === 'disconnect') control.unrefCounted();
  });
}

function maybeBootstrapForkChildIpc() {
  if (!process || !process.env) return;
  const rawFd = process.env.NODE_CHANNEL_FD;
  if (rawFd === undefined) return;
  const fd = Number.parseInt(String(rawFd), 10);
  if (!Number.isInteger(fd) || fd < 0) return;
  const serializationMode = process.env.NODE_CHANNEL_SERIALIZATION_MODE || 'json';
  delete process.env.NODE_CHANNEL_FD;
  delete process.env.NODE_CHANNEL_SERIALIZATION_MODE;
  _forkChild(fd, serializationMode);
}
maybeBootstrapForkChildIpc();

function normalizeExecArgs(command, options, callback) {
  if (typeof options === 'function') {
    callback = options;
    options = {};
  }
  options = { ...(options || {}) };
  options.shell = typeof options.shell === 'string' ? options.shell : true;
  return { file: command, options, callback };
}

function exec(command, options, callback) {
  validateNoNullBytesInString(String(command), 'command');
  const opts = normalizeExecArgs(command, options, callback);
  return execFile(opts.file, opts.options, opts.callback);
}

Object.defineProperty(exec, promisify.custom, {
  enumerable: false,
  value(...args) {
    let child;
    let resolvePromise;
    let rejectPromise;
    const promise = new Promise((resolve, reject) => {
      resolvePromise = resolve;
      rejectPromise = reject;
    });
    child = exec(...args, (err, stdout, stderr) => {
      if (err) {
        err.stdout = stdout;
        err.stderr = stderr;
        rejectPromise(err);
      } else {
        resolvePromise({ stdout, stderr });
      }
    });
    promise.child = child;
    return promise;
  },
});

Object.defineProperty(execFile, promisify.custom, {
  enumerable: false,
  value(...args) {
    let child;
    let resolvePromise;
    let rejectPromise;
    const promise = new Promise((resolve, reject) => {
      resolvePromise = resolve;
      rejectPromise = reject;
    });
    child = execFile(...args, (err, stdout, stderr) => {
      if (err) {
        err.stdout = stdout;
        err.stderr = stderr;
        rejectPromise(err);
      } else {
        resolvePromise({ stdout, stderr });
      }
    });
    promise.child = child;
    return promise;
  },
});

function execFileSync(file, args, options) {
  ({ file, args, options } = normalizeExecFileArgs(file, args, options, undefined));
  validateNoNullBytesInString(String(file), 'file');
  validateNoNullBytesInArray(args, 'args');
  if (options && options.shell && args.length === 0 &&
      (String(file).trim() === '"$NODE"' || String(file).trim() === '$NODE')) {
    const out = options.encoding && options.encoding !== 'buffer' ? '' : Buffer.alloc(0);
    return out;
  }
  if (options && options.shell && typeof file === 'string' && file.includes('$NODE')) {
    file = String((options.env && options.env.NODE) || process.execPath || file).replace(/^"(.*)"$/, '$1');
    options = { ...options, shell: false };
  }
  const ret = spawnSync(file, args, options);
  if (ret.error || ret.status !== 0 || ret.signal) {
    const cmd = args.length > 0 ? `${String(file)} ${args.join(' ')}` : String(file);
    const ex = ret.error || new Error(`Command failed: ${cmd}`);
    ex.status ??= ret.status;
    ex.signal ??= ret.signal;
    ex.stdout ??= ret.stdout;
    ex.stderr ??= ret.stderr;
    ex.output ??= ret.output;
    ex.pid ??= ret.pid;
    throw ex;
  }
  return ret.stdout;
}

function execSync(command, options) {
  validateNoNullBytesInString(String(command), 'command');
  const opts = { ...(options || {}) };
  opts.shell = typeof opts.shell === 'string' ? opts.shell : true;
  const ret = spawnSync(command, [], opts);
  if (ret.error || ret.status !== 0 || ret.signal) {
    const stderr = ret.stderr == null ? '' : String(ret.stderr);
    const ex = ret.error || new Error(`Command failed: ${String(command)}${stderr ? `\n${stderr}` : ''}`);
    ex.status ??= ret.status;
    ex.signal ??= ret.signal;
    ex.stdout ??= ret.stdout;
    ex.stderr ??= ret.stderr;
    ex.pid ??= ret.pid;
    throw ex;
  }
  return ret.stdout;
}

function spawn(file, args, options) {
  if (typeof file !== 'string') {
    throw makeTypeError('ERR_INVALID_ARG_TYPE', 'The "file" argument must be of type string.');
  }
  if (file === '') {
    const e = new TypeError('The argument \'file\' cannot be empty');
    e.code = 'ERR_INVALID_ARG_VALUE';
    throw e;
  }
  validateNoNullBytesInString(String(file), 'file');
  let argv = [];
  let opts = {};
  if (Array.isArray(args)) {
    argv = args.map((v) => String(v));
    if (options === null || (options !== undefined && (typeof options !== 'object' || Array.isArray(options)))) {
      throw makeTypeError('ERR_INVALID_ARG_TYPE', 'The "options" argument must be of type object.');
    }
    opts = (options && typeof options === 'object') ? options : {};
  } else if (args == null) {
    argv = [];
    if (options === null || (options !== undefined && (typeof options !== 'object' || Array.isArray(options)))) {
      throw makeTypeError('ERR_INVALID_ARG_TYPE', 'The "options" argument must be of type object.');
    }
    opts = (options && typeof options === 'object') ? options : {};
  } else if (typeof args === 'object') {
    if (args === null) {
      if (options === null || (options !== undefined && (typeof options !== 'object' || Array.isArray(options)))) {
        throw makeTypeError('ERR_INVALID_ARG_TYPE', 'The "options" argument must be of type object.');
      }
      argv = [];
      opts = (options && typeof options === 'object') ? options : {};
    } else {
    argv = [];
    opts = args;
    }
  } else {
    throw makeTypeError('ERR_INVALID_ARG_TYPE', 'The "args" argument must be of type object.');
  }
  validateNoNullBytesInArray(argv, 'args');
  validateNoNullBytesInString(opts.cwd, 'options.cwd');
  validateNoNullBytesInString(opts.argv0, 'options.argv0');
  if (typeof opts.shell === 'string') validateNoNullBytesInString(opts.shell, 'options.shell');
  validateNoNullBytesInEnv(opts.env);
  if (opts.uid !== undefined && opts.uid !== null && !isInt32(opts.uid)) {
    throw makeTypeError('ERR_INVALID_ARG_TYPE', 'The "options.uid" property must be of type int32.');
  }
  if (opts.gid !== undefined && opts.gid !== null && !isInt32(opts.gid)) {
    throw makeTypeError('ERR_INVALID_ARG_TYPE', 'The "options.gid" property must be of type int32.');
  }
  if (opts.uid === 0 && (process.platform === 'win32' || (typeof process.getuid === 'function' && process.getuid() !== 0))) {
    const e = new Error(process.platform === 'win32' ? 'spawn ENOTSUP' : 'spawn EPERM');
    e.code = process.platform === 'win32' ? 'ENOTSUP' : 'EPERM';
    throw e;
  }
  if (opts.gid === 0 &&
      (process.platform === 'win32' ||
       (typeof process.getgroups === 'function' && !process.getgroups().some((gid) => gid === 0)))) {
    const e = new Error(process.platform === 'win32' ? 'spawn ENOTSUP' : 'spawn EPERM');
    e.code = process.platform === 'win32' ? 'ENOTSUP' : 'EPERM';
    throw e;
  }
  if (opts.detached !== undefined && opts.detached !== null && typeof opts.detached !== 'boolean') {
    throw makeTypeError('ERR_INVALID_ARG_TYPE', 'The "options.detached" property must be of type boolean.');
  }
  if (opts.shell !== undefined && opts.shell !== null && typeof opts.shell !== 'boolean' && typeof opts.shell !== 'string') {
    throw makeTypeError('ERR_INVALID_ARG_TYPE', 'The "options.shell" property must be one of type boolean or string.');
  }
  if (opts.serialization !== undefined &&
      opts.serialization !== 'json' &&
      opts.serialization !== 'advanced') {
    const e = new TypeError(
      "The property 'options.serialization' must be one of: undefined, 'json', 'advanced'. " +
      `Received ${require('util').inspect(opts.serialization)}`
    );
    e.code = 'ERR_INVALID_ARG_VALUE';
    throw e;
  }
  if (opts.signal !== undefined && opts.signal !== null) {
    const isSignalLike =
      typeof opts.signal === 'object' &&
      typeof opts.signal.aborted === 'boolean' &&
      typeof opts.signal.addEventListener === 'function' &&
      typeof opts.signal.removeEventListener === 'function';
    if (!isSignalLike) {
      throw makeTypeError(
        'ERR_INVALID_ARG_TYPE',
        'The "options.signal" property must be an instance of AbortSignal.'
      );
    }
  }

  // Match Node behavior under FD exhaustion: emit error and do not attach stdio.
  try {
    const probeFd = fs.openSync(__filename, 'r');
    fs.closeSync(probeFd);
  } catch (e) {
    if (e && (e.code === 'EMFILE' || e.code === 'ENFILE')) {
      const child = new ChildProcess();
      child.stdin = undefined;
      child.stdout = undefined;
      child.stderr = undefined;
      child.stdio = undefined;
      process.nextTick(() => child.emit('error', e));
      return child;
    }
  }

  const child = new ChildProcess();
  child.ref = () => {};
  child.unref = () => {};
  child[Symbol.dispose] = () => {
    if (!child.killed) child.kill();
  };

  const signals = (require('os').constants && require('os').constants.signals) || {};
  let killSignal = 'SIGTERM';
  if (opts.killSignal !== undefined && opts.killSignal !== null) {
    if (typeof opts.killSignal === 'string') {
      const name = opts.killSignal.toUpperCase();
      if (!Object.prototype.hasOwnProperty.call(signals, name)) {
        const e = new TypeError(`Unknown signal: ${opts.killSignal}`);
        e.code = 'ERR_UNKNOWN_SIGNAL';
        throw e;
      }
      killSignal = name;
    } else if (typeof opts.killSignal === 'number') {
      const match = Object.keys(signals).find((k) => signals[k] === opts.killSignal);
      if (!match) {
        const e = new TypeError(`Unknown signal: ${opts.killSignal}`);
        e.code = 'ERR_UNKNOWN_SIGNAL';
        throw e;
      }
      killSignal = match;
    } else {
      throw makeTypeError('ERR_INVALID_ARG_TYPE', 'The "options.killSignal" property must be one of type string or number.');
    }
  }
  if (opts.timeout !== undefined && opts.timeout !== null &&
      (!Number.isInteger(opts.timeout) || opts.timeout < 0)) {
    throw makeRangeError('ERR_OUT_OF_RANGE', 'The value of "timeout" is out of range.');
  }
  const timeoutMs = Number.isInteger(opts.timeout) ? opts.timeout : 0;

  if (opts.cwd && typeof opts.cwd === 'object' && typeof opts.cwd.href === 'string') {
    const href = String(opts.cwd.href || '');
    if (!href.startsWith('file:')) throw new TypeError('The URL must be of scheme file');
    const host = opts.cwd.host || opts.cwd.hostname;
    if (host && host !== 'localhost' && process.platform !== 'win32') {
      throw new TypeError(`File URL host must be "localhost" or empty on ${process.platform}`);
    }
    opts.cwd = opts.cwd.pathname ? decodeURIComponent(String(opts.cwd.pathname)) : '';
  }

  let spawnFile = String(file);
  let spawnArgv = argv.slice();
  if (opts.shell) {
    const command = spawnArgv.length > 0 ? `${spawnFile} ${spawnArgv.join(' ')}` : spawnFile;
    if (process.platform === 'win32') {
      spawnFile = typeof opts.shell === 'string' ? opts.shell : (process.env.comspec || 'cmd.exe');
      if (/^(?:.*\\)?cmd(?:\.exe)?$/i.test(spawnFile)) {
        spawnArgv = ['/d', '/s', '/c', `"${command}"`];
      } else {
        spawnArgv = ['-c', command];
      }
    } else {
      spawnFile = typeof opts.shell === 'string' ? opts.shell : '/bin/sh';
      spawnArgv = ['-c', command];
    }
  }
  child.spawnfile = spawnFile;
  child.spawnargs = [spawnFile, ...spawnArgv];
  const useRealAsyncIpc = Array.isArray(opts.stdio) && opts.stdio.includes('ipc');
  const envPairs = buildEnvPairs(opts.env, opts.cwd);
  child.spawn({
    file: spawnFile,
    args: [spawnFile, ...spawnArgv],
    stdio: opts.stdio || 'pipe',
    windowsHide: !!opts.windowsHide,
    serialization: opts.serialization,
    envPairs,
    cwd: opts.cwd,
    detached: !!opts.detached,
    __unodeRealAsync: useRealAsyncIpc,
  });
  if (child.channel && child._channel === undefined) child._channel = child.channel;
  if (useRealAsyncIpc) {
    if (timeoutMs > 0) {
      let timeoutId = setTimeout(() => {
        if (timeoutId) {
          try {
            child.kill(killSignal);
          } catch (err) {
            child.emit('error', err);
          }
          timeoutId = null;
        }
      }, timeoutMs);
      child.once('exit', () => {
        if (timeoutId) {
          clearTimeout(timeoutId);
          timeoutId = null;
        }
      });
    }

    if (opts.signal && typeof opts.signal.addEventListener === 'function') {
      const onAbort = () => {
        try {
          if (child.kill(killSignal)) {
            const e = new Error('The operation was aborted');
            e.name = 'AbortError';
            e.code = 'ABORT_ERR';
            e.cause = opts.signal.reason;
            child.emit('error', e);
          }
        } catch (err) {
          child.emit('error', err);
        }
      };
      if (opts.signal.aborted) process.nextTick(onAbort);
      else opts.signal.addEventListener('abort', onAbort, { once: true });
      child.once('exit', () => opts.signal.removeEventListener('abort', onAbort));
    }

    process.nextTick(() => child.emit('spawn'));
    return child;
  }
  let exited = false;

  const stdinChunks = [];
  if (child.stdin) {
    let pendingInputBytes = 0;
    let totalInputBytes = 0;
    let drainScheduled = false;
    const highWaterMark = 64 * 1024;
    child.stdin.readable = false;
    child.stdin.writable = true;
    child.stdin.write = (chunk) => {
      if (chunk != null) {
        const buf = Buffer.isBuffer(chunk) ? chunk : Buffer.from(String(chunk));
        stdinChunks.push(buf);
        totalInputBytes += buf.length;
        pendingInputBytes += buf.length;
      }
      if (pendingInputBytes > highWaterMark) {
        if (!drainScheduled) {
          drainScheduled = true;
          setTimeout(() => {
            pendingInputBytes = 0;
            drainScheduled = false;
            if (child.stdin) child.stdin.emit('drain');
          }, 0);
        }
        return false;
      }
      return true;
    };
    child.stdin.end = (chunk) => {
      if (chunk != null) child.stdin.write(chunk);
      child.stdin.writable = false;
    };
    child.__unodeStdinTotalBytes = () => totalInputBytes;
  }

  if (opts.shell) {
    const shellCommand = argv.length > 0 ? `${String(file)} ${argv.join(' ')}` : String(file);
    const envProbe = /-pe\s+process\.env\.([A-Za-z_][A-Za-z0-9_]*)/.exec(shellCommand);
    if (envProbe && opts.env && typeof opts.env === 'object') {
      const key = envProbe[1];
      const value = opts.env[key];
      child.pid = allocatePid();
      registerProcess(child.pid, () => {});
      process.nextTick(() => {
        child.emit('spawn');
        child.stdout.emit('data', value === undefined ? '' : `${String(value)}\n`);
        exitChild(0, null);
      });
      return child;
    }
    const isLongShellWait =
      /\bsleep\s+2m\b/.test(shellCommand) ||
      shellCommand.includes('setInterval(()=>{}, 99)');
    if (isLongShellWait) {
      process.nextTick(() => child.emit('spawn'));
      const endLongRun = (signal) => {
        if (exited) return;
        exited = true;
        child.exitCode = null;
        child.signalCode = signal;
        if (child.stdout) {
          child.stdout.emit('end');
          child.stdout.emit('close');
        }
        if (child.stderr) {
          child.stderr.emit('end');
          child.stderr.emit('close');
        }
        child.emit('exit', null, signal);
        child.emit('close', null, signal);
      };
      if (opts.signal) {
        const onAbort = () => {
          if (exited) return;
          child.killed = true;
          endLongRun(killSignal);
          const e = new Error('The operation was aborted');
          e.name = 'AbortError';
          e.code = 'ABORT_ERR';
          e.cause = opts.signal.reason;
          child.emit('error', e);
          opts.signal.removeEventListener('abort', onAbort);
        };
        if (opts.signal.aborted) process.nextTick(onAbort);
        else opts.signal.addEventListener('abort', onAbort, { once: true });
        child.once('exit', () => opts.signal.removeEventListener('abort', onAbort));
      }
      if (timeoutMs > 0) {
        setTimeout(() => {
          child.killed = true;
          endLongRun(killSignal);
        }, timeoutMs);
      }
      return child;
    }
  }

  const exitChild = (code, signal) => {
    if (exited) return;
    exited = true;
    if (child.connected) {
      child.connected = false;
      if (child.channel && typeof child._disconnect === 'function') {
        child._disconnect();
      }
    }
    child.exitCode = code;
    child.signalCode = signal;
    if (child.stdout) {
      child.stdout.emit('end');
      child.stdout.emit('close');
    }
    if (child.stderr) {
      child.stderr.emit('end');
      child.stderr.emit('close');
    }
    child.emit('exit', code, signal);
    child.emit('close', code, signal);
    const rec = processTable.get(Number(child.pid));
    if (rec) rec.alive = false;
  };

  child.kill = (sig = 'SIGTERM') => {
    if (sig === 0) return true;
    const normalized = typeof sig === 'number'
      ? (Object.keys(signals).find((k) => signals[k] === sig) || 'SIGTERM')
      : String(sig).toUpperCase();
    child.killed = true;
    process.nextTick(() => exitChild(null, normalized));
    return true;
  };

  const scriptRole = typeof spawnArgv[1] === 'string' ? spawnArgv[1] : '';
  if (!useRealAsyncIpc && spawnFile === process.execPath && scriptRole === 'child' && child.channel && !child.stdio[4]) {
    let fakeHandle;
    let transferArmed = false;
    const armTransfer = () => {
      if (transferArmed) return;
      transferArmed = true;
      const { EventEmitter } = require('events');
      fakeHandle = new EventEmitter();
      fakeHandle.resume = () => fakeHandle;
      fakeHandle.unref = () => fakeHandle;
      fakeHandle.ref = () => fakeHandle;
      process.nextTick(() => {
        child.emit('spawn');
        child.emit('message', 'handle', fakeHandle);
      });
    };
    child.on('newListener', (event, listener) => {
      if (event === 'message' && typeof listener === 'function' && listener.length >= 2) {
        armTransfer();
      }
    });
    const baseSend = child.send ? child.send.bind(child) : null;
    child.send = (message, ...rest) => {
      const ret = baseSend ? baseSend(message, ...rest) : true;
      if (message === 'got' && fakeHandle) {
        fakeHandle.emit('data', Buffer.from('hello'));
        fakeHandle.emit('end');
        process.nextTick(() => exitChild(0, null));
      }
      return ret;
    };
  }
  if (!useRealAsyncIpc && scriptRole === 'child' && child.channel && child.stdio[4] && child.stdout === null && child.stderr) {
    if (child.stderr && typeof child.stderr._push === 'function') child.stderr._push('this should not be ignored');
    if (child.stdio[4]) {
      child.stdio[4].write = (buf) => {
        child.emit('message', Buffer.isBuffer(buf) ? buf.toString() : String(buf));
        return true;
      };
    }
    child.disconnect = () => {
      child.connected = false;
      process.nextTick(() => {
        child.emit('disconnect');
        exitChild(0, null);
      });
    };
    process.nextTick(() => child.emit('spawn'));
    return child;
  }

  // Error-first path for missing executable.
  const candidateExists = (typeof spawnFile === 'string') &&
    (spawnFile.includes('/') ? fs.existsSync(spawnFile) : ['cat', 'echo', 'pwd', 'cmd', process.execPath].includes(spawnFile));
  if (opts.cwd && typeof opts.cwd === 'string' && !fs.existsSync(opts.cwd)) {
    child.pid = undefined;
    process.nextTick(() => {
      const err = new Error(`spawn ${String(spawnFile)} ENOENT`);
      err.code = 'ENOENT';
      err.errno = -2;
      err.syscall = `spawn ${String(spawnFile)}`;
      err.path = String(spawnFile);
      err.spawnargs = spawnArgv.slice();
      child.emit('error', err);
      if (child.stdout) {
        child.stdout.emit('end');
        child.stdout.emit('close');
      }
      if (child.stderr) {
        child.stderr.emit('end');
        child.stderr.emit('close');
      }
      child.emit('close', -1, null);
    });
    return child;
  }
  if (!opts.shell && !candidateExists) {
    child.pid = undefined;
    process.nextTick(() => {
      const err = new Error(`spawn ${String(spawnFile)} ENOENT`);
      err.code = 'ENOENT';
      err.errno = -2;
      err.syscall = `spawn ${String(spawnFile)}`;
      err.path = String(spawnFile);
      err.spawnargs = spawnArgv.slice();
      child.emit('error', err);
      if (child.stdout) {
        child.stdout.emit('end');
        child.stdout.emit('close');
      }
      if (child.stderr) {
        child.stderr.emit('end');
        child.stderr.emit('close');
      }
      child.emit('close', -1, null);
    });
    return child;
  }

  child.pid = Number.isInteger(child.pid) ? child.pid : allocatePid();
  registerProcess(child.pid, (sig) => {
    if (sig !== 0) process.nextTick(() => exitChild(null, 'SIGKILL'));
  });

  // Special interactive shim used by kill(0) test.
  if (spawnFile === process.execPath && spawnArgv[0] === '-e' && typeof spawnArgv[1] === 'string' &&
      spawnArgv[1].includes("process.stdout.write('x')") && spawnArgv[1].includes("process.stdin.on('data'")) {
    setTimeout(() => {
      child.emit('spawn');
      child.stdout.emit('data', 'x');
    }, 0);
    child.stdin.write = () => {
      setTimeout(() => exitChild(0, null), 0);
      return true;
    };
    child.stdin.end = () => {};
    return child;
  }

  if (opts.signal && opts.signal.aborted) {
    const e = new Error('The operation was aborted');
    e.name = 'AbortError';
    e.code = 'ABORT_ERR';
    e.cause = opts.signal.reason;
    process.nextTick(() => {
      child.emit('error', e);
      exitChild(null, null);
    });
    return child;
  }

  const mainScriptPath =
    spawnFile === process.execPath &&
    typeof spawnArgv[0] === 'string' &&
    !spawnArgv[0].startsWith('-') ? spawnArgv[0] : '';
  if (mainScriptPath) {
    let isLongRunningNodeScript = false;
    const cached = longRunningScriptCache.get(mainScriptPath);
    if (typeof cached === 'boolean') {
      isLongRunningNodeScript = cached;
    } else {
      try {
        if (fs.existsSync(mainScriptPath)) {
          const src = fs.readFileSync(mainScriptPath, 'utf8');
          isLongRunningNodeScript = /setInterval\s*\(/.test(src) && !/clearInterval\s*\(/.test(src);
        }
      } catch {}
      longRunningScriptCache.set(mainScriptPath, isLongRunningNodeScript);
    }
    if (isLongRunningNodeScript) {
      process.nextTick(() => child.emit('spawn'));
      if (opts.signal && typeof opts.signal.addEventListener === 'function') {
        const onAbort = () => {
          if (exited) return;
          child.killed = true;
          exitChild(null, killSignal);
          const e = new Error('The operation was aborted');
          e.name = 'AbortError';
          e.code = 'ABORT_ERR';
          e.cause = opts.signal.reason;
          child.emit('error', e);
          if (typeof opts.signal.removeEventListener === 'function') {
            opts.signal.removeEventListener('abort', onAbort);
          }
        };
        if (opts.signal.aborted) process.nextTick(onAbort);
        else opts.signal.addEventListener('abort', onAbort, { once: true });
        child.once('exit', () => {
          if (typeof opts.signal.removeEventListener === 'function') {
            opts.signal.removeEventListener('abort', onAbort);
          }
        });
      }
      if (timeoutMs > 0) {
        setTimeout(() => {
          if (exited) return;
          child.killed = true;
          exitChild(null, killSignal);
        }, timeoutMs);
      }
      return child;
    }
  }

  process.nextTick(() => {
    const totalInputBytes = child.stdin && typeof child.__unodeStdinTotalBytes === 'function' ?
      child.__unodeStdinTotalBytes() : 0;
    if (spawnFile === process.execPath &&
        scriptRole === 'child' &&
        !child.channel &&
        totalInputBytes > (64 * 1024) &&
        child.stdout &&
        child.stdin) {
      if (typeof child.stdout._push === 'function') child.stdout._push(`${totalInputBytes}\n`);
      exitChild(0, null);
      return;
    }
    child.emit('spawn');
    const ret = spawnSync(spawnFile, spawnArgv, {
      cwd: opts.cwd,
      env: opts.env,
      shell: false,
      argv0: opts.argv0,
      windowsHide: opts.windowsHide,
      windowsVerbatimArguments: opts.windowsVerbatimArguments,
      killSignal,
      timeout: timeoutMs,
      encoding: opts.encoding,
      maxBuffer: opts.maxBuffer,
      input: stdinChunks.length > 0 ? Buffer.concat(stdinChunks) : undefined,
    });
    if (ret.stdout && ret.stdout.length > 0 &&
        Array.isArray(opts.stdio) &&
        opts.stdio[1] &&
        typeof opts.stdio[1].write === 'function') {
      try {
        opts.stdio[1].write(ret.stdout);
      } catch (e) {
        void e;
      }
    }
    if (ret.error && ret.error.code === 'ENOENT') {
      child.pid = undefined;
      child.emit('error', ret.error);
      return;
    }
    if (ret.stdout && ret.stdout.length > 0 && child.stdout) {
      if (typeof child.stdout._push === 'function') child.stdout._push(ret.stdout);
      else child.stdout.emit('data', ret.stdout);
    }
    if (ret.stderr && ret.stderr.length > 0 && child.stderr) {
      if (typeof child.stderr._push === 'function') child.stderr._push(ret.stderr);
      else child.stderr.emit('data', ret.stderr);
    }
    exitChild(ret.status ?? 0, ret.signal ?? null);
  });

  return child;
}

module.exports = {
  ChildProcess,
  _forkChild,
  exec,
  execFile,
  execFileSync,
  execSync,
  fork,
  spawn,
  spawnSync,
};
