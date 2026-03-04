'use strict';
const EventEmitter = require('events');
const os = require('os');
const assert = require('assert');
const net = require('net');
const dgram = require('dgram');
const { inspect } = require('util');
const { StringDecoder } = require('string_decoder');
const { ErrnoException } = require('internal/errors');
const { Process } = internalBinding('process_wrap');
const { Pipe, constants: PipeConstants } = internalBinding('pipe_wrap');
const { TCP } = internalBinding('tcp_wrap');
const { UDP } = internalBinding('udp_wrap');
const {
  WriteWrap,
  kReadBytesOrError,
  kArrayBufferOffset,
  kLastWriteWasAsync,
  streamBaseState,
} = internalBinding('stream_wrap');

const MAX_HANDLE_RETRANSMISSIONS = 3;
const kPendingMessages = Symbol('kPendingMessages');
const kJSONBuffer = Symbol('kJSONBuffer');
const kStringDecoder = Symbol('kStringDecoder');

const childProcessSerialization = {
  json: {
    initMessageChannel(channel) {
      channel[kJSONBuffer] = '';
      channel[kStringDecoder] = undefined;
      channel.buffering = false;
    },
    *parseChannelMessages(channel, readData) {
      if (!readData || readData.length === 0) return;
      if (channel[kStringDecoder] === undefined) {
        channel[kStringDecoder] = new StringDecoder('utf8');
      }
      const chunks = channel[kStringDecoder].write(Buffer.from(readData)).split('\n');
      const numCompleteChunks = chunks.length - 1;
      const incompleteChunk = chunks[numCompleteChunks];
      if (numCompleteChunks === 0) {
        channel[kJSONBuffer] += incompleteChunk;
      } else {
        chunks[0] = channel[kJSONBuffer] + chunks[0];
        for (let i = 0; i < numCompleteChunks; i++) {
          yield JSON.parse(chunks[i]);
        }
        channel[kJSONBuffer] = incompleteChunk;
      }
      channel.buffering = channel[kJSONBuffer].length !== 0;
    },
    writeChannelMessage(channel, req, message, handle) {
      const string = JSON.stringify(message) + '\n';
      return channel.writeUtf8String(req, string, handle);
    },
  },
};
childProcessSerialization.advanced = childProcessSerialization.json;

function formatReceived(v) {
  if (v == null) return ` Received ${v}`;
  if (typeof v === 'function') return ` Received function ${v.name || '<anonymous>'}`;
  if (typeof v === 'object') {
    if (v.constructor && v.constructor.name) {
      return ` Received an instance of ${v.constructor.name}`;
    }
    return ` Received ${typeof v}`;
  }
  let shown = String(v).slice(0, 25);
  if (String(v).length > 25) shown += '...';
  if (typeof v === 'string') shown = `'${shown}'`;
  return ` Received type ${typeof v} (${shown})`;
}

function makeTypeError(code, message) {
  const e = new TypeError(message);
  e.code = code;
  return e;
}

function makeError(code, message) {
  const e = new Error(message);
  e.code = code;
  return e;
}

function makeArgTypeError(name, type, value, prop = false) {
  const label = prop ? `The "${name}" property` : `The "${name}" argument`;
  return makeTypeError('ERR_INVALID_ARG_TYPE',
                       `${label} must be of type ${type}.${formatReceived(value)}`);
}

function makeArrayTypeError(name, value, prop = false) {
  const label = prop ? `The "${name}" property` : `The "${name}" argument`;
  return makeTypeError('ERR_INVALID_ARG_TYPE',
                       `${label} must be an instance of Array.${formatReceived(value)}`);
}

function makeMissingArgsError(name) {
  const e = new TypeError(`The "${name}" argument must be specified`);
  e.code = 'ERR_MISSING_ARGS';
  return e;
}

function toBuffer(v) {
  if (Buffer.isBuffer(v)) return v;
  if (typeof v === 'string') return Buffer.from(v, 'utf8');
  if (v && typeof v === 'object' && typeof v.byteLength === 'number') {
    try { return Buffer.from(v.buffer, v.byteOffset || 0, v.byteLength); } catch {}
  }
  return Buffer.alloc(0);
}

function stdioStringToArray(stdio, channel) {
  const options = [];
  switch (stdio) {
    case 'ignore':
    case 'overlapped':
    case 'pipe':
      options.push(stdio, stdio, stdio);
      break;
    case 'inherit':
      options.push(0, 1, 2);
      break;
    default:
      throw makeTypeError('ERR_INVALID_ARG_VALUE', `The argument 'stdio' is invalid. Received ${String(stdio)}`);
  }
  if (channel) options.push(channel);
  return options;
}

function getValidStdio(stdio, sync) {
  let ipc;
  let ipcFd;

  if (typeof stdio === 'string') {
    stdio = stdioStringToArray(stdio);
  } else if (!Array.isArray(stdio)) {
    throw makeTypeError('ERR_INVALID_ARG_VALUE', `The argument 'stdio' is invalid. Received ${String(stdio)}`);
  }

  while (stdio.length < 3) stdio.push(undefined);

  const mapped = stdio.map((item, i) => {
    item ??= i < 3 ? 'pipe' : 'ignore';

    if (item === 'ignore') return { type: 'ignore' };

    if (item === 'pipe' || item === 'overlapped' || (typeof item === 'number' && item < 0)) {
      const pipeType = item === 'overlapped' ? 'overlapped' : 'pipe';
      const handle = new Pipe(PipeConstants.SOCKET);
      return {
        type: pipeType,
        handle,
        readable: i === 0,
        writable: i !== 0,
      };
    }

    if (item === 'ipc') {
      if (sync || ipc !== undefined) {
        if (sync) throw makeError('ERR_IPC_SYNC_FORK', 'IPC cannot be used with synchronous forks');
        throw makeError('ERR_IPC_ONE_PIPE', 'Child process can have only one IPC pipe');
      }
      ipcFd = i;
      ipc = new Pipe(PipeConstants.IPC);
      return { type: 'pipe', handle: ipc, ipc: true };
    }

    if (item === 'inherit') return { type: 'inherit', fd: i };
    if (item === process.stdin) return { type: 'fd', fd: 0 };
    if (item === process.stdout) return { type: 'fd', fd: 1 };
    if (item === process.stderr) return { type: 'fd', fd: 2 };
    if (typeof item === 'number' || (item && typeof item.fd === 'number')) {
      return { type: 'fd', fd: typeof item === 'number' ? item : item.fd };
    }
    if (item && (item.handle || item._handle)) {
      return { type: 'wrap', handle: item.handle || item._handle, _stdio: item };
    }
    if (ArrayBuffer.isView(item) || typeof item === 'string') {
      if (!sync) {
        throw makeTypeError('ERR_INVALID_SYNC_FORK_INPUT', `Invalid stdio input: ${String(item)}`);
      }
      return { type: 'pipe', readable: i === 0, writable: i !== 0 };
    }

    throw makeTypeError('ERR_INVALID_ARG_VALUE', `The argument 'stdio' is invalid. Received ${String(item)}`);
  });

  return { stdio: mapped, ipc, ipcFd };
}

class Control extends EventEmitter {
  constructor(channel) {
    super();
    this._channel = channel;
    this._refs = 0;
    this[kPendingMessages] = [];
  }

  refCounted() {
    this._refs++;
    if (this._refs === 1 && this._channel && typeof this._channel.ref === 'function') this._channel.ref();
  }

  unrefCounted() {
    this._refs--;
    if (this._refs <= 0 && this._channel && typeof this._channel.unref === 'function') this._channel.unref();
  }

  ref() {
    if (this._channel && typeof this._channel.ref === 'function') this._channel.ref();
  }

  unref() {
    if (this._channel && typeof this._channel.unref === 'function') this._channel.unref();
  }

  get fd() {
    return this._channel ? this._channel.fd : undefined;
  }
}

const INTERNAL_PREFIX = 'NODE_';
function isInternalMessage(message) {
  return (message !== null &&
          typeof message === 'object' &&
          typeof message.cmd === 'string' &&
          message.cmd.length > INTERNAL_PREFIX.length &&
          String(message.cmd).startsWith(INTERNAL_PREFIX));
}

const handleConversion = {
  'net.Native': {
    simultaneousAccepts: true,
    send(message, handle) {
      return handle;
    },
    got(message, handle, emit) {
      emit(handle);
    },
  },
  'net.Server': {
    simultaneousAccepts: true,
    send(message, server) {
      return server && server._handle;
    },
    got(message, handle, emit) {
      const server = new net.Server();
      server.listen(handle, () => emit(server));
    },
  },
  'net.Socket': {
    send(message, socket, options) {
      if (!socket || !socket._handle) return undefined;
      const handle = socket._handle;
      if (!options.keepOpen) {
        handle.onread = () => {};
        socket._handle = null;
        if (typeof socket.setTimeout === 'function') socket.setTimeout(0);
      }
      return handle;
    },
    postSend(message, handle, options, callback, targetRef) {
      if (handle && !options.keepOpen) {
        if (targetRef) {
          assert(!targetRef._pendingMessage);
          targetRef._pendingMessage = {
            callback,
            message,
            handle,
            options,
            retransmissions: 0,
          };
        } else if (typeof handle.close === 'function') {
          handle.close();
        }
      }
    },
    got(message, handle, emit) {
      const socket = new net.Socket({ handle, readable: true, writable: true });
      emit(socket);
    },
  },
  'dgram.Native': {
    simultaneousAccepts: false,
    send(message, handle) {
      return handle;
    },
    got(message, handle, emit) {
      emit(handle);
    },
  },
  'dgram.Socket': {
    simultaneousAccepts: false,
    send(message, socket) {
      return socket && socket._handle;
    },
    got(message, handle, emit) {
      emit(handle);
    },
  },
};

function setupChannel(target, channel, serializationMode) {
  const control = new Control(channel);
  target.channel = control;
  target._handleQueue = null;
  target._pendingMessage = null;

  const wire = childProcessSerialization[serializationMode || 'json'] ||
               childProcessSerialization.json;
  const { initMessageChannel, parseChannelMessages, writeChannelMessage } = wire;

  function closePendingHandle() {
    if (!target._pendingMessage) return;
    target._pendingMessage.handle.close();
    target._pendingMessage = null;
  }

  function emit(eventName, message, handle) {
    if (eventName === 'internalMessage' || target.listenerCount('message')) {
      try {
        target.emit(eventName, message, handle);
      } catch (err) {
        throw err;
      }
      return;
    }
    target.channel[kPendingMessages].push([eventName, message, handle]);
  }

  function handleMessage(message, handle, internal) {
    if (!target.channel) return;
    const eventName = internal ? 'internalMessage' : 'message';
    process.nextTick(emit, eventName, message, handle);
  }

  target.on('newListener', () => {
    process.nextTick(() => {
      if (!target.channel || !target.listenerCount('message')) return;
      const messages = target.channel[kPendingMessages];
      target.channel[kPendingMessages] = [];
      for (let i = 0; i < messages.length; i++) {
        target.emit(messages[i][0], messages[i][1], messages[i][2]);
      }
    });
  });

  initMessageChannel(channel);
  channel.pendingHandle = null;

  let pendingHandle = null;
  channel.onread = function onread(arrayBuffer) {
    // Node core receives this from C++; our native wrapper may require an
    // explicit acceptPendingHandle() call for NODE_HANDLE payloads.
    const recvHandle = channel.pendingHandle;
    channel.pendingHandle = null;

    if (arrayBuffer) {
      const nread = streamBaseState[kReadBytesOrError] || arrayBuffer.byteLength;
      const offset = streamBaseState[kArrayBufferOffset] || 0;
      const pool = new Uint8Array(arrayBuffer, offset, nread);
      if (recvHandle) pendingHandle = recvHandle;

      try {
        for (const message of parseChannelMessages(channel, pool)) {
          if (isInternalMessage(message)) {
            if (message.cmd === 'NODE_HANDLE') {
              if (!pendingHandle && typeof channel.acceptPendingHandle === 'function') {
                pendingHandle = channel.acceptPendingHandle();
              }
              handleMessage(message, pendingHandle, true);
              pendingHandle = null;
            } else {
              handleMessage(message, undefined, true);
            }
          } else {
            handleMessage(message, undefined, false);
          }
        }
      } catch (e) {
        throw e;
      }
    } else {
      this.buffering = false;
      target.disconnect();
      channel.onread = () => {};
      channel.close();
      target.channel = null;
      maybeClose(target);
    }
  };

  target.on('internalMessage', (message, handle) => {
    if (!message || typeof message !== 'object') return;

    if (message.cmd === 'NODE_HANDLE_ACK' || message.cmd === 'NODE_HANDLE_NACK') {
      if (target._pendingMessage) {
        if (message.cmd === 'NODE_HANDLE_ACK') {
          closePendingHandle();
        } else if (target._pendingMessage.retransmissions++ === MAX_HANDLE_RETRANSMISSIONS) {
          closePendingHandle();
        }
      }

      assert(Array.isArray(target._handleQueue));
      const queue = target._handleQueue;
      target._handleQueue = null;

      if (target._pendingMessage) {
        target._send(
          target._pendingMessage.message,
          target._pendingMessage.handle,
          target._pendingMessage.options,
          target._pendingMessage.callback,
        );
      }
      for (let i = 0; i < queue.length; i++) {
        const args = queue[i];
        target._send(args.message, args.handle, args.options, args.callback);
      }
      if (!target.connected && target.channel && !target._handleQueue) target._disconnect();
      return;
    }

    if (message.cmd !== 'NODE_HANDLE') return;
    if (!handle) {
      return target._send({ cmd: 'NODE_HANDLE_NACK' }, null, true);
    }

    // ACK should not throw to user space.
    target._send({ cmd: 'NODE_HANDLE_ACK' }, null, true);

    const obj = handleConversion[message.type];
    if (!obj) return;
    obj.got.call(target, message, handle, (converted) => {
      handleMessage(message.msg, converted, isInternalMessage(message.msg));
    });
  });

  target.send = function send(message, handle, options, callback) {
    if (typeof handle === 'function') {
      callback = handle;
      handle = undefined;
      options = undefined;
    } else if (typeof options === 'function') {
      callback = options;
      options = undefined;
    } else if (options !== undefined && (options === null || typeof options !== 'object')) {
      throw makeArgTypeError('options', 'object', options);
    }
    options = { swallowErrors: false, ...(options || {}) };

    if (!this.connected) {
      const ex = makeError('ERR_IPC_CHANNEL_CLOSED', 'Channel closed');
      if (typeof callback === 'function') process.nextTick(callback, ex);
      else process.nextTick(() => this.emit('error', ex));
      return false;
    }
    return this._send(message, handle, options, callback);
  };

  target._send = function _send(message, handle, options, callback) {
    assert(this.connected || this.channel);

    if (typeof options === 'boolean') options = { swallowErrors: options };
    options = { swallowErrors: false, ...(options || {}) };

    if (message === undefined) {
      throw makeMissingArgsError('message');
    }
    const valid = typeof message === 'string' ||
      typeof message === 'number' ||
      typeof message === 'boolean' ||
      (message !== null && typeof message === 'object');
    if (!valid) {
      const e = new TypeError(
        'The "message" argument must be one of type string, object, number, or boolean.' +
        formatReceived(message)
      );
      e.code = 'ERR_INVALID_ARG_TYPE';
      throw e;
    }

    let obj;
    if (handle) {
      message = {
        cmd: 'NODE_HANDLE',
        type: null,
        msg: message,
      };

      if (handle instanceof net.Socket) {
        message.type = 'net.Socket';
      } else if (handle instanceof net.Server) {
        message.type = 'net.Server';
      } else if (handle instanceof TCP || handle instanceof Pipe) {
        message.type = 'net.Native';
      } else if (handle instanceof dgram.Socket) {
        message.type = 'dgram.Socket';
      } else if (handle instanceof UDP) {
        message.type = 'dgram.Native';
      } else {
        throw makeTypeError('ERR_INVALID_HANDLE_TYPE', 'This handle type cannot be sent');
      }

      if (this._handleQueue) {
        this._handleQueue.push({ callback, handle, options, message: message.msg });
        return this._handleQueue.length === 1;
      }

      obj = handleConversion[message.type];
      handle = obj.send.call(target, message, handle, options);
      if (!handle) message = message.msg;
    } else if (this._handleQueue &&
               !(message && (message.cmd === 'NODE_HANDLE_ACK' ||
                             message.cmd === 'NODE_HANDLE_NACK'))) {
      this._handleQueue.push({ callback, handle: null, options, message });
      return this._handleQueue.length === 1;
    }

    const req = new WriteWrap();
    const rc = writeChannelMessage(channel, req, message, handle);
    if (rc === 0) {
      if (handle) {
        this._handleQueue ||= [];
        if (obj && obj.postSend) obj.postSend(message, handle, options, callback, target);
      }

      const wasAsyncWrite = streamBaseState[kLastWriteWasAsync];
      if (wasAsyncWrite) {
        req.oncomplete = () => {
          control.unrefCounted();
          if (typeof callback === 'function') callback(null);
        };
        control.refCounted();
      } else if (typeof callback === 'function') {
        process.nextTick(callback, null);
      }
    } else {
      if (obj && obj.postSend) obj.postSend(message, handle, options, callback);
      if (!options.swallowErrors) {
        const ex = makeError('ERR_IPC_CHANNEL_CLOSED', 'Channel closed');
        if (typeof callback === 'function') process.nextTick(callback, ex);
        else process.nextTick(() => this.emit('error', ex));
      }
    }
    const queueSize =
      (typeof channel.writeQueueSize === 'number' && Number.isFinite(channel.writeQueueSize))
        ? channel.writeQueueSize
        : 0;
    return queueSize < (65536 * 2);
  };

  target.connected = true;

  target.disconnect = function disconnect() {
    if (!this.connected) {
      this.emit('error', makeError('ERR_IPC_DISCONNECTED', 'IPC channel is already disconnected'));
      return;
    }
    this.connected = false;
    if (!this._handleQueue) this._disconnect();
  };

  target._disconnect = function _disconnect() {
    if (!channel || !target.channel) return;
    target.channel = null;
    closePendingHandle();
    let fired = false;
    const finish = () => {
      if (fired) return;
      fired = true;
      if (channel && typeof channel.close === 'function') channel.close();
      target.emit('disconnect');
    };
    if (channel.buffering) {
      this.once('message', finish);
      this.once('internalMessage', finish);
      return;
    }
    process.nextTick(finish);
  }

  channel.readStart();
  return control;
}

function flushStdio(subprocess) {
  const stdio = subprocess && subprocess.stdio;
  if (!Array.isArray(stdio)) return;
  for (let i = 0; i < stdio.length; i++) {
    const stream = stdio[i];
    if (!stream || !stream.readable) continue;
    if (typeof stream.resume === 'function') stream.resume();
  }
}

function maybeClose(subprocess) {
  if (!subprocess) return;
  subprocess._closesGot++;
  if (subprocess._closesGot === subprocess._closesNeeded) {
    subprocess.emit('close', subprocess.exitCode, subprocess.signalCode);
  }
}

function makeHandleMethodOverridable(handle, name) {
  if (!handle || typeof handle[name] !== 'function') return;
  try {
    const original = handle[name].bind(handle);
    Object.defineProperty(handle, name, {
      configurable: true,
      enumerable: true,
      writable: true,
      value: original,
    });
  } catch {}
}

function createSocket(pipe, readable) {
  const socket = net.Socket({ handle: pipe, readable });
  if (socket && socket._handle) {
    makeHandleMethodOverridable(socket._handle, 'readStart');
    makeHandleMethodOverridable(socket._handle, 'readStop');
  }
  return socket;
}

class ChildProcess extends EventEmitter {
  constructor() {
    super();
    this._closesNeeded = 1;
    this._closesGot = 0;
    this.connected = false;
    this.signalCode = null;
    this.exitCode = null;
    this.killed = false;
    this.spawnfile = null;
    this.spawnargs = [];
    this.pid = undefined;
    this.stdin = null;
    this.stdout = null;
    this.stderr = null;
    this.stdio = [];
    this._handle = new Process();
    this._didExit = false;
    this._handle.onexit = (exitCode, signalCode) => {
      if (this._didExit) return;
      this._didExit = true;
      // Ensure parent-side IPC pipe does not keep the loop alive after child exit.
      if (this.connected) {
        this.connected = false;
        if (typeof this._disconnect === 'function') this._disconnect();
      } else if (this.channel && typeof this._disconnect === 'function') {
        this._disconnect();
      }
      if (this.stdin && typeof this.stdin.destroy === 'function') this.stdin.destroy();
      if (this._handle && typeof this._handle.close === 'function') this._handle.close();
      this._handle = null;
      if (signalCode) {
        this.signalCode = signalCode;
        this.exitCode = null;
      } else {
        this.exitCode = exitCode == null ? null : exitCode;
        this.signalCode = null;
      }
      if (typeof exitCode === 'number' && exitCode < 0) {
        const syscall = this.spawnfile ? `spawn ${this.spawnfile}` : 'spawn';
        let systemCode = null;
        try {
          systemCode = require('util').getSystemErrorName(exitCode);
        } catch {}
        const err = new Error(`${syscall} ${systemCode || exitCode}`);
        err.code = systemCode || exitCode;
        err.errno = exitCode;
        err.syscall = syscall;
        if (this.spawnfile) err.path = this.spawnfile;
        err.spawnargs = Array.isArray(this.spawnargs) ? this.spawnargs.slice(1) : [];
        this.emit('error', err);
      } else {
        this.emit('exit', this.exitCode, this.signalCode);
      }
      process.nextTick(flushStdio, this);
      maybeClose(this);
    };
  }

  spawn(options) {
    if (options === null || typeof options !== 'object') {
      throw makeArgTypeError('options', 'object', options);
    }
    if (options.serialization !== undefined &&
        options.serialization !== 'json' &&
        options.serialization !== 'advanced') {
      const e = new TypeError(
        "The property 'options.serialization' must be one of: undefined, 'json', 'advanced'. " +
        `Received ${inspect(options.serialization)}`
      );
      e.code = 'ERR_INVALID_ARG_VALUE';
      throw e;
    }
    if (options.envPairs !== undefined && !Array.isArray(options.envPairs)) {
      throw makeArrayTypeError('options.envPairs', options.envPairs, true);
    }
    if (typeof options.file !== 'string') {
      throw makeArgTypeError('options.file', 'string', options.file, true);
    }
    if (options.args !== undefined && !Array.isArray(options.args)) {
      throw makeArrayTypeError('options.args', options.args, true);
    }

    const stdioData = getValidStdio(options.stdio || 'pipe', false);
    const ipc = stdioData.ipc;
    const ipcFd = stdioData.ipcFd;
    const stdio = stdioData.stdio;
    options.stdio = stdio;

    const serialization = options.serialization || 'json';
    if (ipc !== undefined) {
      if (options.envPairs === undefined) options.envPairs = [];
      else if (!Array.isArray(options.envPairs)) throw makeArrayTypeError('options.envPairs', options.envPairs, true);
      options.envPairs.push(`NODE_CHANNEL_FD=${ipcFd}`);
      options.envPairs.push(`NODE_CHANNEL_SERIALIZATION_MODE=${serialization}`);
    }

    this.spawnfile = options.file;
    this.spawnargs = Array.isArray(options.args) ? options.args.slice() : [];
    this._didExit = false;

    const err = this._handle.spawn(options);
    if (err) {
      process.nextTick(() => {
        if (this._handle && typeof this._handle.onexit === 'function') {
          this._handle.onexit(err, null);
        }
      });
    }
    this.pid = err ? undefined : (this._handle ? this._handle.pid : undefined);

    this.stdio = [];
    for (let i = 0; i < stdio.length; i++) {
      const stream = stdio[i];
      if (stream.type === 'ignore' || stream.type === 'inherit' || stream.type === 'fd') {
        this.stdio.push(null);
        continue;
      }
      if (stream.ipc) {
        this._closesNeeded++;
        this.stdio.push(null);
        continue;
      }
      if (stream.type === 'wrap') {
        if (stream.handle && typeof stream.handle.readStop === 'function') {
          stream.handle.reading = false;
          stream.handle.readStop();
        }
        if (stream._stdio && typeof stream._stdio.pause === 'function') {
          stream._stdio.pause();
        }
        if (stream._stdio && typeof stream._stdio === 'object') {
          stream._stdio.readableFlowing = false;
          if (stream._stdio._readableState && typeof stream._stdio._readableState === 'object') {
            stream._stdio._readableState.reading = false;
          }
        }
        this.stdio.push(stream._stdio || null);
        continue;
      }
      if (stream.handle) {
        const socket = createSocket(this.pid !== 0 ? stream.handle : null, i > 0);
        this.stdio.push(socket);
        if (i > 0 && this.pid !== 0) {
          this._closesNeeded++;
          socket.on('close', () => {
            maybeClose(this);
          });
        }
        continue;
      }
      this.stdio.push(null);
    }

    this.stdin = this.stdio[0] || null;
    this.stdout = this.stdio[1] || null;
    this.stderr = this.stdio[2] || null;

    if (ipc) setupChannel(this, ipc, serialization);

    return err;
  }

  kill(sig) {
    const signals = (os.constants && os.constants.signals) || {};
    let normalized = 'SIGTERM';
    if (sig !== undefined && sig !== null && sig !== 0) {
      if (typeof sig === 'string') {
        const name = String(sig).toUpperCase();
        if (!Object.prototype.hasOwnProperty.call(signals, name)) {
          const e = new TypeError(`Unknown signal: ${sig}`);
          e.code = 'ERR_UNKNOWN_SIGNAL';
          throw e;
        }
        normalized = name;
      } else if (typeof sig === 'number') {
        const found = Object.keys(signals).find((k) => signals[k] === sig);
        if (!found) {
          const e = new TypeError(`Unknown signal: ${sig}`);
          e.code = 'ERR_UNKNOWN_SIGNAL';
          throw e;
        }
        normalized = found;
      } else {
        throw makeArgTypeError('sig', 'string or number', sig);
      }
    }

    if (sig === 0) return true;

    const err = this._handle && typeof this._handle.kill === 'function' ?
      this._handle.kill(sig == null ? normalized : sig) : -1;
    if (err === 0) {
      this.killed = true;
      return true;
    }
    return false;
  }

  ref() {}
  unref() {}
}

ChildProcess.prototype[Symbol.dispose] = function symbolDispose() {
  if (!this.killed) this.kill();
};

function spawnSync(options) {
  const binding = internalBinding('spawn_sync');
  if (!binding || typeof binding.spawn !== 'function') {
    return {
      pid: 0,
      output: [null, Buffer.alloc(0), Buffer.alloc(0)],
      stdout: Buffer.alloc(0),
      stderr: Buffer.alloc(0),
      status: null,
      signal: null,
      error: undefined,
    };
  }
  const result = binding.spawn(options || {}) || {};
  const out = Array.isArray(result.output) ? result.output : [null, Buffer.alloc(0), Buffer.alloc(0)];
  const output = [null, toBuffer(out[1]), toBuffer(out[2])];
  if (options && options.encoding && options.encoding !== 'buffer') {
    output[1] = output[1].toString(options.encoding);
    output[2] = output[2].toString(options.encoding);
  }
  result.output = output;
  result.stdout = output[1];
  result.stderr = output[2];
  if (result.error) {
    result.error = new ErrnoException(result.error, 'spawnSync ' + options.file);
    result.error.path = options.file;
    result.error.spawnargs = Array.isArray(options.args) ? options.args.slice(1) : [];
  }
  return result;
}

module.exports = {
  ChildProcess,
  getValidStdio,
  setupChannel,
  stdioStringToArray,
  spawnSync,
};
