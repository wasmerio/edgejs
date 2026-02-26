'use strict';

const EventEmitter = require('events');

class MessagePort {
  postMessage(_value, transferList) {
    if (Array.isArray(transferList)) {
      for (const item of transferList) {
        if (!item || typeof item !== 'object') continue;
        const symbols = Object.getOwnPropertySymbols(item);
        for (const sym of symbols) {
          const key = String(sym);
          if (/untransferable/i.test(key) && item[sym]) {
            const err = new Error('Object could not be cloned.');
            err.name = 'DataCloneError';
            err.code = 25;
            throw err;
          }
        }
      }
    }
  }
  close() {}
}

class MessageChannel {
  constructor() {
    this.port1 = new MessagePort();
    this.port2 = new MessagePort();
  }
}

const isMainThread = process.env.UNODE_IS_WORKER_THREAD !== '1';

class Worker extends EventEmitter {
  constructor(filename, options) {
    super();
    const opts = options && typeof options === 'object' ? options : {};
    const execArgv = Array.isArray(opts.execArgv) ? opts.execArgv.slice() :
      (Array.isArray(process.execArgv) ? process.execArgv.slice() : []);
    const spawn = require('child_process').spawn;
    const env = { ...process.env, UNODE_IS_WORKER_THREAD: '1' };
    this._child = spawn(process.execPath, [...execArgv, filename], { env });
    this.threadId = this._child.pid || 0;
    this.stdout = opts.stdout ? this._child.stdout : undefined;
    this.stderr = opts.stderr ? this._child.stderr : undefined;
    if (!opts.stdout && this._child.stdout && typeof this._child.stdout.pipe === 'function' &&
        process.stdout && typeof process.stdout.write === 'function') {
      this._child.stdout.pipe(process.stdout);
    }
    if (!opts.stderr && this._child.stderr && typeof this._child.stderr.pipe === 'function' &&
        process.stderr && typeof process.stderr.write === 'function') {
      this._child.stderr.pipe(process.stderr);
    }
    this._child.on('exit', (code) => this.emit('exit', code));
    this._child.on('error', (err) => this.emit('error', err));
  }

  terminate() {
    if (this._child && typeof this._child.kill === 'function') this._child.kill();
    return Promise.resolve(0);
  }
}

module.exports = {
  MessageChannel,
  Worker,
  isMainThread,
};
