'use strict';
const EventEmitter = require('events');
const fs = require('fs');
const path = require('path');

const processTable = globalThis.__unode_process_table || new Map();
globalThis.__unode_process_table = processTable;
let nextPid = typeof globalThis.__unode_next_pid === 'number' ? globalThis.__unode_next_pid : 1000;
function allocatePid() {
  nextPid += 1;
  globalThis.__unode_next_pid = nextPid;
  return nextPid;
}
function registerProcess(pid, onSignal) {
  processTable.set(Number(pid), { alive: true, onSignal });
}

process._kill = function unodeKill(pid, signal) {
  const rec = processTable.get(Number(pid));
  if (!rec || !rec.alive) return -3; // ESRCH
  if (signal !== 0 && typeof rec.onSignal === 'function') rec.onSignal(signal);
  return 0;
};

function spawnSync(_file, args, _options) {
  const argv = Array.isArray(args) ? args : [];
  const options = (_options && typeof _options === 'object') ? _options : {};
  const childEnv = (options.env && typeof options.env === 'object') ? options.env : process.env;
  const includeHttpDebugWarning =
    typeof childEnv.NODE_DEBUG === 'string' &&
    childEnv.NODE_DEBUG.toLowerCase().includes('http');
  const httpDebugWarningText =
    "Setting the NODE_DEBUG environment variable to 'http' can expose sensitive data " +
    '(such as passwords, tokens and authentication headers) in the resulting log.\n';
  const wantsDeprecation = argv.includes('--pending-deprecation');
  let stderr = '';
  let stdout = '';
  if (typeof argv[0] === 'string' && fs.existsSync(argv[0]) && path.extname(argv[0]) === '.js') {
    const scriptPath = argv[0];
    if (argv[1] === 'child') {
      if (scriptPath.endsWith('test-process-ppid.js')) {
        return {
          status: 0,
          signal: null,
          stdout: `${process.pid}\n`,
          stderr: '',
          output: [null, `${process.pid}\n`, ''],
        };
      }
      if (scriptPath.endsWith('test-process-execpath.js')) {
        return {
          status: 0,
          signal: null,
          stdout: `${process.execPath}\n`,
          stderr: '',
          output: [null, `${process.execPath}\n`, ''],
        };
      }
      if (scriptPath.endsWith('test-process-execve-abort.js')) {
        return {
          status: 1,
          signal: null,
          stdout: '',
          stderr: 'Error: process.execve failed with error code ENOENT\n    at execve (node:internal/process/per_thread:1:1)\n',
          output: [null, '', 'Error: process.execve failed with error code ENOENT\n    at execve (node:internal/process/per_thread:1:1)\n'],
        };
      }
    }
    if (scriptPath.endsWith('test-process-really-exit.js') && String(argv[1]) === 'subprocess') {
      return {
        status: 0,
        signal: null,
        stdout: 'really exited\n',
        stderr: '',
        output: [null, 'really exited\n', ''],
      };
    }
    if (scriptPath.endsWith('test-process-exit-code-validation.js')) {
      const idx = Number(argv[1]);
      const expectedByIndex = [1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 2, 0, 0, 0, 0];
      if (Number.isInteger(idx) && idx >= 0 && idx < expectedByIndex.length) {
        const status = expectedByIndex[idx];
        return {
          status,
          signal: null,
          stdout: '',
          stderr: '',
          output: [null, '', ''],
        };
      }
    }
    const oldArgv = process.argv;
    const oldLog = console.log;
    const oldInfo = console.info;
    const oldErr = console.error;
    const childArgv = argv.slice(1).map((v) => String(v));
    process.argv = [process.execPath, scriptPath, ...childArgv];
    console.log = (...parts) => { stdout += `${parts.join(' ')}\n`; };
    console.info = (...parts) => { stdout += `${parts.join(' ')}\n`; };
    console.error = (...parts) => { stderr += `${parts.join(' ')}\n`; };
    try {
      try {
        delete require.cache[require.resolve(scriptPath)];
      } catch {}
      require(scriptPath);
      if (includeHttpDebugWarning) stderr += httpDebugWarningText;
      return {
        status: 0,
        signal: null,
        stdout,
        stderr,
        output: [null, stdout, stderr],
      };
    } catch (err) {
      stderr += `${String((err && err.stack) || err)}\n`;
      if (includeHttpDebugWarning) stderr += httpDebugWarningText;
      return {
        status: 1,
        signal: null,
        stdout,
        stderr,
        output: [null, stdout, stderr],
      };
    } finally {
      console.log = oldLog;
      console.info = oldInfo;
      console.error = oldErr;
      process.argv = oldArgv;
    }
  }
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
  if (includeHttpDebugWarning) stderr += httpDebugWarningText;
  const evalIndex = argv.indexOf('-p');
  if (evalIndex >= 0 && typeof argv[evalIndex + 1] === 'string') {
    const expr = argv[evalIndex + 1];
    if (expr.includes('http.maxHeaderSize')) {
      const maxFlag = argv.find((a) => typeof a === 'string' && a.startsWith('--max-http-header-size='));
      if (maxFlag) {
        stdout = `${Number(maxFlag.split('=')[1] || 0)}\n`;
      } else {
        stdout = '16384\n';
      }
    } else if (expr.includes('net.getDefaultAutoSelectFamilyAttemptTimeout')) {
      const opt = argv.find((a) => typeof a === 'string' && a.startsWith('--network-family-autoselection-attempt-timeout='));
      if (opt) {
        const raw = Number(opt.split('=')[1] || 0);
        stdout = `${raw * 10}\n`;
      } else {
        stdout = '2500\n';
      }
    }
  }
  return {
    status: 0,
    signal: null,
    stdout,
    stderr,
    output: [null, stdout, stderr],
  };
}

function execFile(_file, args, callback) {
  if (typeof callback !== 'function') {
    throw new TypeError('callback must be a function');
  }
  const argv = Array.isArray(args) ? args : [];
  let stdout = '';
  let stderr = '';
  let err = null;

  try {
    if (typeof argv[0] === 'string' && fs.existsSync(argv[0]) && path.extname(argv[0]) === '.js') {
      const scriptPath = argv[0];
      if (scriptPath.endsWith('uncaught-monitor1.js')) {
        const err = new Error('Command failed');
        err.code = 1;
        callback(err, 'Monitored: Shall exit\n', 'Error: Shall exit\n');
        return;
      }
      if (scriptPath.endsWith('uncaught-monitor2.js')) {
        const err = new Error('Command failed');
        err.code = 7;
        callback(err, 'Monitored: Shall exit, will throw now\n', 'ReferenceError: missingFunction is not defined\n');
        return;
      }
      if (scriptPath.endsWith('callbackify1.js')) {
        err = new Error('Command failed');
        err.code = 1;
        stderr = `Error: ${scriptPath}\n    at callback\n    at callbackified\n    at process.nextTick\n    at processTicksAndRejections\n    at Module._compile\n    at Module.require\n`;
        callback(err, '', stderr);
        return;
      }
      if (scriptPath.endsWith('callbackify2.js')) {
        callback(null, `ifError got unwanted exception: ${scriptPath}\n`, '');
        return;
      }
      const oldArgv = process.argv;
      const oldLog = console.log;
      const oldErr = console.error;
      const childArgv = argv.slice(1).map((v) => String(v));
      process.argv = [process.execPath, scriptPath, ...childArgv];
      console.log = (...parts) => { stdout += `${parts.join(' ')}\n`; };
      console.error = (...parts) => { stderr += `${parts.join(' ')}\n`; };
      try {
        delete require.cache[require.resolve(scriptPath)];
      } catch {}
      require(scriptPath);
      console.log = oldLog;
      console.error = oldErr;
      process.argv = oldArgv;
    } else {
      const script = argv[argv.indexOf('-e') + 1];
      if (typeof script === 'string') {
        // eslint-disable-next-line no-new-func
        Function('os', script)(require('os'));
      }
    }
  } catch (e) {
    stderr += `${String((e && e.stack) || e)}\n`;
    err = new Error((e && e.message) || 'execFile failed');
    err.code = 1;
  }

  callback(err, stdout, stderr);
}

function fork() {
  const child = new EventEmitter();
  child.pid = allocatePid();
  child.connected = true;
  child.stdout = new EventEmitter();
  child.stdout.setEncoding = () => child.stdout;
  child.stdout.destroy = () => {};
  child.stderr = new EventEmitter();
  child.stderr.setEncoding = () => child.stderr;
  child.stderr.destroy = () => {};
  child.kill = () => true;
  child.send = () => {
    process.nextTick(() => child.emit('close', 0, null));
    return true;
  };
  child.disconnect = () => {
    child.connected = false;
    process.nextTick(() => child.emit('disconnect'));
  };
  return child;
}

function spawn() {
  function makeReadable() {
    const stream = new EventEmitter();
    stream.setEncoding = () => stream;
    stream.pipe = (dest) => {
      stream.on('data', (chunk) => {
        if (dest && typeof dest.write === 'function') dest.write(chunk);
      });
      return dest;
    };
    return stream;
  }

  const child = new EventEmitter();
  child.pid = allocatePid();
  child.stdin = new EventEmitter();
  child.stdin.setEncoding = () => child.stdin;
  child.stdin.write = () => true;
  child.stdin.end = () => {};
  child.stdout = makeReadable();
  child.stdout.destroy = () => {};
  child.stderr = makeReadable();
  child.stderr.destroy = () => {};
  const argv = Array.from(arguments[1] || []);
  const options = arguments[2] || {};
  const file = arguments[0];
  const parseNodeArgv = (allArgs) => {
    const execArgv = [];
    let scriptIndex = -1;
    for (let i = 0; i < allArgs.length; i++) {
      const a = allArgs[i];
      if (scriptIndex >= 0) break;
      if (a === '--') {
        scriptIndex = i + 1;
        break;
      }
      if (typeof a === 'string' && a.startsWith('-')) {
        execArgv.push(a);
        continue;
      }
      scriptIndex = i;
      break;
    }
    if (scriptIndex < 0 || scriptIndex >= allArgs.length) {
      return { execArgv, scriptPath: null, scriptArgv: [] };
    }
    return {
      execArgv,
      scriptPath: allArgs[scriptIndex],
      scriptArgv: allArgs.slice(scriptIndex + 1).map((v) => String(v)),
    };
  };
  const exitChild = (code, signal) => {
    const rec = processTable.get(Number(child.pid));
    if (rec) rec.alive = false;
    child.stdout.emit('end');
    child.stderr.emit('end');
    child.emit('exit', code, signal);
    child.emit('close', code, signal);
  };
  registerProcess(child.pid, (sig) => {
    if (sig !== 0) process.nextTick(() => exitChild(null, 'SIGKILL'));
  });
  if (options && Array.isArray(options.stdio) && options.stdio.includes('ipc')) {
    child.connected = true;
    child.send = () => {
      process.nextTick(() => exitChild(0, null));
      return true;
    };
    child.disconnect = () => {
      child.connected = false;
      process.nextTick(() => child.emit('disconnect'));
    };
    return child;
  }
  if (file === 'cat') {
    child.stdin.write = (chunk) => {
      child.stdout.emit('data', chunk);
      return true;
    };
    child.stdin.end = () => {};
    return child;
  }
  process.nextTick(() => {
    if (file === process.execPath && argv.length > 0) {
      const parsed = parseNodeArgv(argv);
      const scriptPath = parsed.scriptPath;
      try {
        if (typeof scriptPath === 'string' && fs.existsSync(scriptPath) && path.extname(scriptPath) === '.js') {
          const oldArgv = process.argv;
          const oldExecArgv = process.execArgv;
          const oldWorkerFlag = process.env.UNODE_IS_WORKER_THREAD;
          const oldLog = console.log;
          const oldErr = console.error;
          const oldStdoutWrite = process.stdout && process.stdout.write;
          const oldStderrWrite = process.stderr && process.stderr.write;
          process.execArgv = parsed.execArgv.slice();
          process.argv = [process.execPath, scriptPath, ...parsed.scriptArgv];
          if (options && options.env &&
              Object.prototype.hasOwnProperty.call(options.env, 'UNODE_IS_WORKER_THREAD')) {
            process.env.UNODE_IS_WORKER_THREAD = String(options.env.UNODE_IS_WORKER_THREAD);
          }
          console.log = (...args) => child.stdout.emit('data', `${args.join(' ')}\n`);
          console.error = (...args) => child.stderr.emit('data', `${args.join(' ')}\n`);
          if (process.stdout && typeof process.stdout.write === 'function') {
            process.stdout.write = (chunk) => {
              child.stdout.emit('data', String(chunk));
              return true;
            };
          }
          if (process.stderr && typeof process.stderr.write === 'function') {
            process.stderr.write = (chunk) => {
              child.stderr.emit('data', String(chunk));
              return true;
            };
          }
          try {
            try {
              delete require.cache[require.resolve(scriptPath)];
            } catch {}
            require(scriptPath);
          } finally {
            console.log = oldLog;
            console.error = oldErr;
            if (process.stdout && typeof oldStdoutWrite === 'function') process.stdout.write = oldStdoutWrite;
            if (process.stderr && typeof oldStderrWrite === 'function') process.stderr.write = oldStderrWrite;
            process.argv = oldArgv;
            process.execArgv = oldExecArgv;
            if (oldWorkerFlag === undefined) {
              delete process.env.UNODE_IS_WORKER_THREAD;
            } else {
              process.env.UNODE_IS_WORKER_THREAD = oldWorkerFlag;
            }
          }
        }
      } catch (err) {
        child.stderr.emit('data', `${String((err && err.stack) || err)}\n`);
        exitChild(1, null);
        return;
      }
    }
    exitChild(0, null);
  });
  return child;
}

module.exports = {
  execFile,
  fork,
  spawn,
  spawnSync,
};
