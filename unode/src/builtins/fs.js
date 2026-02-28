'use strict';

const { EventEmitter } = require('events');
const binding = globalThis.__unode_fs;
const encodingBinding = globalThis.__unode_encoding || null;
let warnedInvalidExistsSyncPath = false;
const activeRequests = globalThis.__unode_active_requests || [];
globalThis.__unode_active_requests = activeRequests;
if (!binding) {
  throw new Error('fs builtin requires __unode_fs binding');
}

if (typeof globalThis.TextEncoder === 'undefined') {
  globalThis.TextEncoder = function TextEncoder() {};
  globalThis.TextEncoder.prototype.encode = function encode(s) {
    const str = String(s);
    if (encodingBinding && typeof encodingBinding.encodeUtf8 === 'function') {
      return encodingBinding.encodeUtf8(str);
    }
    const out = [];
    for (let i = 0; i < str.length; i++) {
      let c = str.charCodeAt(i);
      if (c >= 0xd800 && c <= 0xdbff && i + 1 < str.length) {
        const c2 = str.charCodeAt(i + 1);
        if (c2 >= 0xdc00 && c2 <= 0xdfff) {
          c = 0x10000 + ((c - 0xd800) << 10) + (c2 - 0xdc00);
          i++;
        }
      }
      if (c < 0x80) out.push(c);
      else if (c < 0x800) { out.push(0xc0 | (c >> 6)); out.push(0x80 | (c & 0x3f)); }
      else if (c < 0x10000) { out.push(0xe0 | (c >> 12)); out.push(0x80 | ((c >> 6) & 0x3f)); out.push(0x80 | (c & 0x3f)); }
      else { out.push(0xf0 | (c >> 18)); out.push(0x80 | ((c >> 12) & 0x3f)); out.push(0x80 | ((c >> 6) & 0x3f)); out.push(0x80 | (c & 0x3f)); }
    }
    return new Uint8Array(out);
  };
}

function pathTypeError(path) {
  const err = new TypeError('path must be a string, Buffer, or URL. Received ' + (path === null ? 'null' : typeof path));
  err.code = 'ERR_INVALID_ARG_TYPE';
  return err;
}

// Minimal helper for Node test message format (oldPath/newPath)
function invalidArgTypeHelper(input) {
  if (input == null) return ` Received ${input}`;
  if (typeof input === 'function') return ` Received function ${input.name || '<anonymous>'}`;
  if (typeof input === 'object') {
    if (input.constructor && input.constructor.name) return ` Received an instance of ${input.constructor.name}`;
    return ` Received ${typeof input}`;
  }
  const s = String(input);
  return ` Received type ${typeof input} (${s.length > 25 ? s.slice(0, 25) + '...' : s})`;
}

function decodeUtf8FromBytes(view) {
  if (encodingBinding && typeof encodingBinding.decodeUtf8 === 'function') {
    const u8 = view.buffer ? new Uint8Array(view.buffer, view.byteOffset || 0, view.byteLength) : new Uint8Array(view);
    return encodingBinding.decodeUtf8(u8);
  }
  if (typeof TextDecoder !== 'undefined') {
    return new TextDecoder().decode(view);
  }
  const u8 = view.buffer ? new Uint8Array(view.buffer, view.byteOffset || 0, view.byteLength) : new Uint8Array(view);
  let s = '';
  for (let i = 0; i < u8.length; ) {
    const b = u8[i++];
    if (b < 0x80) {
      s += String.fromCharCode(b);
    } else if (b < 0xe0) {
      s += String.fromCharCode(((b & 0x1f) << 6) | (u8[i++] & 0x3f));
    } else if (b < 0xf0) {
      s += String.fromCharCode(((b & 0x0f) << 12) | ((u8[i++] & 0x3f) << 6) | (u8[i++] & 0x3f));
    } else {
      const c = ((b & 0x07) << 18) | ((u8[i++] & 0x3f) << 12) | ((u8[i++] & 0x3f) << 6) | (u8[i++] & 0x3f);
      s += c <= 0xffff ? String.fromCharCode(c) : String.fromCharCode(0xd7c0 + (c >> 10), 0xdc00 + (c & 0x3ff));
    }
  }
  return s;
}

function getValidatedPath(path, argName) {
  if (path == null) {
    const type = 'of type string or an instance of Buffer or URL.' + invalidArgTypeHelper(path);
    const err = new TypeError(argName ? `The "${argName}" argument must be ${type}` : 'path must be a string, Buffer, or URL. Received ' + (path === null ? 'null' : typeof path));
    err.code = 'ERR_INVALID_ARG_TYPE';
    throw err;
  }
  if (typeof path === 'object' && path && path.href !== undefined) {
    const protocol = typeof path.protocol === 'string' ? path.protocol : '';
    if (protocol && protocol !== 'file:') {
      const err = new TypeError('The URL must be of scheme file');
      err.code = 'ERR_INVALID_URL_SCHEME';
      throw err;
    }
    path = path.pathname || path.href;
    if (typeof path === 'string' && path.startsWith('file://')) path = path.slice(7) || '/';
  } else if (typeof path === 'object' && path && typeof path.length === 'number' && path.toString && path.constructor && path.constructor.name === 'Buffer') {
    path = path.toString('utf8');
  } else if (typeof path === 'object' && path && typeof path.byteLength === 'number') {
    path = decodeUtf8FromBytes(path.buffer ? new Uint8Array(path.buffer, path.byteOffset || 0, path.byteLength) : path);
  } else if (typeof path === 'object' && path) {
    const type = 'of type string or an instance of Buffer or URL.' + invalidArgTypeHelper(path);
    const err = new TypeError(argName ? `The "${argName}" argument must be ${type}` : 'path must be a string, Buffer, or URL. Received ' + typeof path);
    err.code = 'ERR_INVALID_ARG_TYPE';
    throw err;
  }
  if (typeof path !== 'string' && typeof path === 'object' && path && path.toString) {
    path = path.toString();
  }
  if (typeof path !== 'string') {
    const type = 'of type string or an instance of Buffer or URL.' + invalidArgTypeHelper(path);
    const err = new TypeError(argName ? `The "${argName}" argument must be ${type}` : 'path must be a string, Buffer, or URL. Received ' + typeof path);
    err.code = 'ERR_INVALID_ARG_TYPE';
    throw err;
  }
  if (path.includes('\u0000')) {
    throw new Error('path must be a string without null bytes');
  }
  return path;
}

function getOptions(options, defaults) {
  if (options == null || typeof options === 'function') {
    return Object.assign({}, defaults);
  }
  if (typeof options === 'string') {
    const o = Object.assign({}, defaults);
    o.encoding = options;
    return o;
  }
  if (typeof options !== 'object') {
    throw new Error('options must be an object or string');
  }
  return Object.assign({}, defaults, options);
}

function stringToFlags(flags) {
  if (typeof flags === 'number') {
    return flags;
  }
  if (flags == null) {
    return binding.O_RDONLY;
  }
  switch (flags) {
    case 'r':
      return binding.O_RDONLY;
    case 'rs':
    case 'sr':
      return binding.O_RDONLY | binding.O_SYNC;
    case 'r+':
      return binding.O_RDWR;
    case 'rs+':
    case 'sr+':
      return binding.O_RDWR | binding.O_SYNC;
    case 'w':
      return binding.O_TRUNC | binding.O_CREAT | binding.O_WRONLY;
    case 'wx':
    case 'xw':
      return binding.O_TRUNC | binding.O_CREAT | binding.O_WRONLY | binding.O_EXCL;
    case 'w+':
      return binding.O_TRUNC | binding.O_CREAT | binding.O_RDWR;
    case 'wx+':
    case 'xw+':
      return binding.O_TRUNC | binding.O_CREAT | binding.O_RDWR | binding.O_EXCL;
    case 'a':
      return binding.O_APPEND | binding.O_CREAT | binding.O_WRONLY;
    case 'ax':
    case 'xa':
      return binding.O_APPEND | binding.O_CREAT | binding.O_WRONLY | binding.O_EXCL;
    case 'as':
    case 'sa':
      return binding.O_APPEND | binding.O_CREAT | binding.O_WRONLY | binding.O_SYNC;
    case 'a+':
      return binding.O_APPEND | binding.O_CREAT | binding.O_RDWR;
    case 'ax+':
    case 'xa+':
      return binding.O_APPEND | binding.O_CREAT | binding.O_RDWR | binding.O_EXCL;
    case 'as+':
    case 'sa+':
      return binding.O_APPEND | binding.O_CREAT | binding.O_RDWR | binding.O_SYNC;
    default:
      throw new Error(`Invalid flag: ${flags}`);
  }
}

function parseFileMode(mode, name, def) {
  if (mode === undefined || mode === null) {
    mode = def;
  }
  if (typeof mode === 'string') {
    if (!/^0o[0-7]+$/.test(mode) && !/^[0-7]+$/.test(mode)) {
      throw new Error(`Invalid ${name}: ${mode}`);
    }
    mode = parseInt(mode.replace(/^0o/, ''), 8);
  }
  if (typeof mode !== 'number' || mode < 0 || mode > 0xFFFF) {
    throw new Error(`Invalid ${name}: ${mode}`);
  }
  return mode >>> 0;
}

const defaultRmOptions = {
  recursive: false,
  force: false,
  retryDelay: 100,
  maxRetries: 0,
};

function validateRmOptionsSync(path, options) {
  const opts = Object.assign({}, defaultRmOptions, options && typeof options === 'object' ? options : {});
  if (typeof opts.recursive !== 'boolean') opts.recursive = false;
  if (typeof opts.retryDelay !== 'number' || opts.retryDelay < 0) opts.retryDelay = 100;
  if (typeof opts.maxRetries !== 'number' || opts.maxRetries < 0) opts.maxRetries = 0;
  return { maxRetries: opts.maxRetries, recursive: opts.recursive, retryDelay: opts.retryDelay };
}

class Dirent {
  constructor(name, type, path) {
    this.name = name;
    this.parentPath = path;
    this._type = type;
  }

  isDirectory() {
    return this._type === binding.UV_DIRENT_DIR;
  }

  isFile() {
    return this._type === binding.UV_DIRENT_FILE;
  }

  isBlockDevice() {
    return this._type === binding.UV_DIRENT_BLOCK;
  }

  isCharacterDevice() {
    return this._type === binding.UV_DIRENT_CHAR;
  }

  isSymbolicLink() {
    return this._type === binding.UV_DIRENT_LINK;
  }

  isFIFO() {
    return this._type === binding.UV_DIRENT_FIFO;
  }

  isSocket() {
    return this._type === binding.UV_DIRENT_SOCKET;
  }
}

// Stats: binding returns 18-element array [dev, mode, nlink, uid, gid, rdev, blksize, ino, size, blocks, atime_sec, atime_nsec, mtime_sec, mtime_nsec, ctime_sec, ctime_nsec, birthtime_sec, birthtime_nsec]
const kMsPerSec = 1000;
const kNsecPerMs = 1000000;
function msFromTimeSpec(sec, nsec) {
  return sec * kMsPerSec + nsec / kNsecPerMs;
}

function Stats(dev, mode, nlink, uid, gid, rdev, blksize, ino, size, blocks, atimeMs, mtimeMs, ctimeMs, birthtimeMs) {
  this.dev = dev;
  this.mode = mode;
  this.nlink = nlink;
  this.uid = uid;
  this.gid = gid;
  this.rdev = rdev;
  this.blksize = blksize;
  this.ino = ino;
  this.size = size;
  this.blocks = blocks;
  this.atimeMs = atimeMs;
  this.mtimeMs = mtimeMs;
  this.ctimeMs = ctimeMs;
  this.birthtimeMs = birthtimeMs;
}

Stats.prototype._checkModeProperty = function (property) {
  return (this.mode & binding.S_IFMT) === property;
};
Stats.prototype.isDirectory = function () { return this._checkModeProperty(binding.S_IFDIR); };
Stats.prototype.isFile = function () { return this._checkModeProperty(binding.S_IFREG); };
Stats.prototype.isBlockDevice = function () { return this._checkModeProperty(binding.S_IFBLK); };
Stats.prototype.isCharacterDevice = function () { return this._checkModeProperty(binding.S_IFCHR); };
Stats.prototype.isSymbolicLink = function () { return this._checkModeProperty(binding.S_IFLNK); };
Stats.prototype.isFIFO = function () { return this._checkModeProperty(binding.S_IFIFO); };
Stats.prototype.isSocket = function () { return this._checkModeProperty(binding.S_IFSOCK); };

function dateFromMs(ms) {
  return new Date(Math.round(Number(ms)));
}
function makeTimeGetter(msProp, storeProp) {
  return function () {
    if (Object.prototype.hasOwnProperty.call(this, storeProp)) return this[storeProp];
    return dateFromMs(this[msProp]);
  };
}
function makeTimeSetter(storeProp) {
  return function (v) { this[storeProp] = v; };
}
Object.defineProperty(Stats.prototype, 'atime', {
  enumerable: true,
  get: makeTimeGetter('atimeMs', '_atimeValue'),
  set: makeTimeSetter('_atimeValue')
});
Object.defineProperty(Stats.prototype, 'mtime', {
  enumerable: true,
  get: makeTimeGetter('mtimeMs', '_mtimeValue'),
  set: makeTimeSetter('_mtimeValue')
});
Object.defineProperty(Stats.prototype, 'ctime', {
  enumerable: true,
  get: makeTimeGetter('ctimeMs', '_ctimeValue'),
  set: makeTimeSetter('_ctimeValue')
});
Object.defineProperty(Stats.prototype, 'birthtime', {
  enumerable: true,
  get: makeTimeGetter('birthtimeMs', '_birthtimeValue'),
  set: makeTimeSetter('_birthtimeValue')
});

function getStatsFromBinding(arr) {
  return new Stats(
    arr[0], arr[1], arr[2], arr[3], arr[4], arr[5], arr[6], arr[7], arr[8], arr[9],
    msFromTimeSpec(arr[10], arr[11]),
    msFromTimeSpec(arr[12], arr[13]),
    msFromTimeSpec(arr[14], arr[15]),
    msFromTimeSpec(arr[16], arr[17])
  );
}

function statSync(path, options) {
  const opts = getOptions(options, { throwIfNoEntry: true });
  path = getValidatedPath(path);
  try {
    const arr = binding.stat(path);
    return getStatsFromBinding(arr);
  } catch (e) {
    if (opts.throwIfNoEntry === false && e.code === 'ENOENT') return undefined;
    throw e;
  }
}

function lstatSync(path, options) {
  const opts = getOptions(options, { throwIfNoEntry: true });
  path = getValidatedPath(path);
  try {
    const arr = binding.lstat(path);
    return getStatsFromBinding(arr);
  } catch (e) {
    if (opts.throwIfNoEntry === false && e.code === 'ENOENT') return undefined;
    throw e;
  }
}

function fstatSync(fd, options) {
  if (typeof fd !== 'number' || Number.isNaN(fd)) {
    const err = new TypeError('The "fd" argument must be of type number. Received ' + (typeof fd === 'symbol' ? 'Symbol' : String(fd)));
    err.code = 'ERR_INVALID_ARG_TYPE';
    throw err;
  }
  const arr = binding.fstat(fd);
  if (arr == null) return undefined;
  return getStatsFromBinding(arr);
}

function existsSync(path) {
  try {
    path = getValidatedPath(path);
  } catch {
    if (!warnedInvalidExistsSyncPath && typeof process?.emitWarning === 'function') {
      warnedInvalidExistsSyncPath = true;
      process.emitWarning(
        'Passing invalid argument types to fs.existsSync is deprecated',
        'DeprecationWarning',
        'DEP0187'
      );
    }
    return false;
  }
  return binding.existsSync(path);
}

function exists(path, callback) {
  if (typeof callback !== 'function') {
    const err = new TypeError('Callback must be a function. Received ' + String(callback));
    err.code = 'ERR_INVALID_ARG_TYPE';
    throw err;
  }
  const schedule = typeof globalThis.queueMicrotask === 'function' ?
    globalThis.queueMicrotask :
    (fn) => fn();
  schedule(() => {
    try {
      path = getValidatedPath(path);
      const ok = binding.existsSync(path);
      callback(ok);
    } catch {
      callback(false);
    }
  });
}

function accessSync(path, mode) {
  const m = mode === undefined ? binding.F_OK : mode;
  path = getValidatedPath(path);
  binding.accessSync(path, m);
}

function openSync(path, flags, mode) {
  path = getValidatedPath(path);
  const f = flags == null ? binding.O_RDONLY : (typeof flags === 'string' ? stringToFlags(flags) : flags);
  const m = mode === undefined || mode === null ? 0o666 : parseFileMode(mode, 'mode', 0o666);
  return binding.open(path, f, m);
}

function closeSync(fd) {
  binding.close(fd);
}

function isArrayBufferView(value) {
  return value != null && typeof value === 'object' && value.buffer instanceof ArrayBuffer && typeof value.byteLength === 'number';
}

function read(fd, buffer, offsetOrOptions, length, position, callback) {
  if (typeof fd !== 'number' || Number.isNaN(fd)) {
    const err = new TypeError('The "fd" argument must be of type number. Received ' + String(fd));
    err.code = 'ERR_INVALID_ARG_TYPE';
    throw err;
  }
  let offset = 0;
  let pos = null;
  if (arguments.length === 3) {
    callback = offsetOrOptions;
    if (typeof buffer === 'object' && buffer !== null && ('buffer' in buffer || 'offset' in buffer)) {
      const opts = buffer;
      buffer = opts.buffer;
      offset = opts.offset ?? 0;
      length = opts.length ?? (buffer ? buffer.byteLength - offset : 0);
      pos = opts.position ?? null;
    } else {
      length = buffer && buffer.byteLength !== undefined ? buffer.byteLength : 0;
    }
  } else if (arguments.length === 4 && typeof length === 'function') {
    callback = length;
    if (typeof offsetOrOptions === 'object' && offsetOrOptions !== null) {
      const opts = offsetOrOptions;
      buffer = opts.buffer;
      offset = opts.offset ?? 0;
      length = opts.length ?? (buffer ? buffer.byteLength - offset : 0);
      pos = opts.position ?? null;
    } else {
      offset = Number(offsetOrOptions) | 0;
      length = buffer ? buffer.byteLength - offset : 0;
    }
  } else if (typeof position === 'function') {
    callback = position;
    offset = Number(offsetOrOptions) | 0;
    length = length | 0;
    pos = undefined;
  } else {
    callback = arguments[5];
    offset = Number(offsetOrOptions) | 0;
    length = length | 0;
    pos = position;
  }
  if (typeof callback !== 'function') {
    const err = new TypeError('The "callback" argument must be a function. Received ' + String(callback));
    err.code = 'ERR_INVALID_ARG_TYPE';
    throw err;
  }
  if (buffer == null || (typeof buffer !== 'string' && (!buffer || (buffer.buffer === undefined && buffer.byteLength === undefined)))) {
    const received = buffer === null ? 'null' : (buffer === undefined ? 'undefined' : 'an instance of Object');
    const err = new TypeError('The "buffer" argument must be an instance of Buffer or ArrayBufferView. Received ' + received);
    err.code = 'ERR_INVALID_ARG_TYPE';
    throw err;
  }
  if (length === undefined) length = buffer.byteLength - offset;
  const positionVal = pos == null ? -1 : Number(pos);
  setImmediateOrSync(() => {
    try {
      const bytesRead = binding.readSync(fd, buffer, offset, length, positionVal);
      callback(null, bytesRead, buffer);
    } catch (e) {
      callback(e);
    }
  });
}

function readSync(fd, buffer, offsetOrOptions, length, position) {
  if (typeof fd !== 'number' || Number.isNaN(fd)) {
    const err = new TypeError('The "fd" argument must be of type number. Received ' + String(fd));
    err.code = 'ERR_INVALID_ARG_TYPE';
    throw err;
  }
  if (!isArrayBufferView(buffer) && !(typeof buffer === 'object' && buffer !== null && typeof buffer.byteLength === 'number')) {
    const received = buffer === null ? 'null' : (buffer === undefined ? 'undefined' : 'an instance of Object');
    const err = new TypeError('The "buffer" argument must be an instance of Buffer, TypedArray, or DataView. Received ' + received);
    err.code = 'ERR_INVALID_ARG_TYPE';
    throw err;
  }
  let offset = offsetOrOptions;
  if (arguments.length <= 3 || typeof offsetOrOptions === 'object') {
    const opts = offsetOrOptions != null ? offsetOrOptions : {};
    offset = opts.offset ?? 0;
    length = opts.length ?? (buffer.byteLength - offset);
    position = opts.position ?? null;
  }
  if (offset === undefined) offset = 0;
  if (length === undefined) length = buffer.byteLength - offset;
  if (position === undefined) position = null;
  offset = Number(offset) | 0;
  length = length | 0;
  if (length === 0) return 0;
  if (buffer.byteLength === 0) {
    const err = new TypeError('buffer is empty and cannot be read');
    err.code = 'ERR_INVALID_ARG_VALUE';
    throw err;
  }
  if (offset < 0 || offset >= buffer.byteLength || length <= 0 || offset + length > buffer.byteLength) {
    const err = new RangeError('Attempt to access memory outside buffer bounds');
    err.code = 'ERR_BUFFER_OUT_OF_BOUNDS';
    throw err;
  }
  const pos = position == null ? -1 : Number(position);
  return binding.readSync(fd, buffer, offset, length, pos);
}

function writeSync(fd, buffer, offsetOrOptions, length, position) {
  if (typeof fd !== 'number' || Number.isNaN(fd)) {
    const err = new TypeError('The "fd" argument must be of type number. Received ' + String(fd));
    err.code = 'ERR_INVALID_ARG_TYPE';
    throw err;
  }
  if (typeof buffer === 'string') {
    return binding.writeSyncString(fd, buffer);
  }
  if (!isArrayBufferView(buffer) && !(typeof buffer === 'object' && buffer !== null && typeof buffer.byteLength === 'number')) {
    const err = new TypeError('The "buffer" argument must be an instance of Buffer or ArrayBufferView or string. Received ' + String(buffer));
    err.code = 'ERR_INVALID_ARG_TYPE';
    throw err;
  }
  let offset = offsetOrOptions;
  if (arguments.length <= 3 || typeof offsetOrOptions === 'object') {
    const opts = offsetOrOptions != null ? offsetOrOptions : {};
    offset = opts.offset ?? 0;
    length = opts.length ?? (buffer.byteLength - offset);
    position = opts.position ?? null;
  }
  if (offset === undefined) offset = 0;
  if (length === undefined) length = buffer.byteLength - offset;
  if (position === undefined) position = null;
  offset = Number(offset) | 0;
  length = length | 0;
  if (length === 0) return 0;
  if (offset < 0 || offset >= buffer.byteLength || length <= 0 || offset + length > buffer.byteLength) {
    const err = new RangeError('Attempt to access memory outside buffer bounds');
    err.code = 'ERR_BUFFER_OUT_OF_BOUNDS';
    throw err;
  }
  const pos = position == null ? -1 : Number(position);
  return binding.writeSync(fd, buffer, offset, length, pos);
}

const setImmediateOrSync = globalThis.setImmediate || function (fn) { fn(); };

function createReadStream(filePath, options) {
  const EventEmitter = require('events');
  const stream = new EventEmitter();
  setImmediateOrSync(() => {
    try {
      const data = readFileSync(filePath, options);
      stream.emit('data', data);
      stream.emit('close');
      stream.emit('end');
    } catch (err) {
      stream.emit('error', err);
    }
  });
  return stream;
}

function makeCallback(cb) {
  if (typeof cb !== 'function') return () => {};
  return function (err) {
    if (arguments.length > 1) cb(err, arguments[1]); else cb(err);
  };
}

function stat(path, options, callback) {
  if (typeof options === 'function') { callback = options; options = {}; }
  callback = makeCallback(callback);
  const p = getValidatedPath(path);
  setImmediateOrSync(() => {
    try {
      const arr = binding.stat(p);
      callback(null, getStatsFromBinding(arr));
    } catch (e) {
      callback(e);
    }
  });
}

function lstat(path, options, callback) {
  if (typeof options === 'function') { callback = options; options = {}; }
  callback = makeCallback(callback);
  const p = getValidatedPath(path);
  setImmediateOrSync(() => {
    try {
      const arr = binding.lstat(p);
      callback(null, getStatsFromBinding(arr));
    } catch (e) {
      callback(e);
    }
  });
}

function fstat(fd, options, callback) {
  if (typeof options === 'function') { callback = options; options = {}; }
  if (typeof fd !== 'number') {
    const err = new TypeError('The "fd" argument must be of type number. Received ' + String(fd));
    err.code = 'ERR_INVALID_ARG_TYPE';
    throw err;
  }
  callback = makeCallback(callback);
  setImmediateOrSync(() => {
    try {
      const arr = binding.fstat(fd);
      callback(null, getStatsFromBinding(arr));
    } catch (e) {
      callback(e);
    }
  });
}

function open(path, flags, mode, callback) {
  if (arguments.length < 3) { callback = flags; flags = 'r'; mode = 0o666; }
  else if (typeof mode === 'function') { callback = mode; mode = 0o666; }
  callback = makeCallback(callback);
  const req = { type: 'FSReqCallback', syscall: 'open' };
  activeRequests.push(req);
  const run = () => {
    try {
      const p = getValidatedPath(path);
      const f = flags == null ? binding.O_RDONLY : (typeof flags === 'string' ? stringToFlags(flags) : flags);
      const m = mode === undefined || mode === null ? 0o666 : parseFileMode(mode, 'mode', 0o666);
      const fd = binding.open(p, f, m);
      callback(null, fd);
    } catch (e) {
      callback(e);
    } finally {
      const drop = () => {
        const i = activeRequests.indexOf(req);
        if (i >= 0) activeRequests.splice(i, 1);
      };
      if (typeof process === 'object' && process && typeof process.nextTick === 'function') {
        process.nextTick(drop);
      } else {
        drop();
      }
    }
  };
  if (typeof process === 'object' && process && typeof process.nextTick === 'function') {
    process.nextTick(run);
  } else {
    run();
  }
}

function close(fd, callback) {
  const cb = typeof callback === 'function' ? callback : () => {};
  setImmediateOrSync(() => {
    try {
      binding.close(fd);
      cb();
    } catch (e) {
      cb(e);
    }
  });
}

function readFileSync(path, options) {
  options = getOptions(options, { flag: 'r' });
  const encoding = options.encoding;
  path = getValidatedPath(path);
  const flags = stringToFlags(options.flag);
  if (encoding === undefined || encoding === 'buffer') {
    const fd = openSync(path, options.flag || 'r');
    const chunks = [];
    const buf = Buffer.allocUnsafe(8192);
    let n;
    try {
      while ((n = binding.readSync(fd, buf, 0, buf.length, -1)) > 0) {
        chunks.push(Buffer.from(buf.subarray(0, n)));
      }
    } finally {
      closeSync(fd);
    }
    return chunks.length === 1 ? chunks[0] : Buffer.concat(chunks);
  }
  if (encoding === 'utf8' || encoding === 'utf-8') {
    return binding.readFileUtf8(path, flags);
  }
  throw new Error('Only utf8 and buffer encoding are supported in this build');
}

function writeFileSync(path, data, options) {
  options = getOptions(options, { encoding: 'utf8', mode: 0o666, flag: 'w' });
  const flag = options.flag || 'w';
  if (typeof data === 'string' && (options.encoding === 'utf8' || options.encoding === 'utf-8')) {
    path = getValidatedPath(path);
    binding.writeFileUtf8(path, data, stringToFlags(flag), parseFileMode(options.mode, 'mode', 0o666));
    return;
  }
  throw new Error('Only string data with utf8 encoding is supported in this build');
}

function mkdirSync(path, options) {
  let mode = 0o777;
  let recursive = false;
  if (typeof options === 'number' || typeof options === 'string') {
    mode = parseFileMode(options, 'mode', 0o777);
  } else if (options && typeof options === 'object') {
    if (options.recursive !== undefined) recursive = Boolean(options.recursive);
    if (options.mode !== undefined) mode = parseFileMode(options.mode, 'options.mode', 0o777);
  }
  path = getValidatedPath(path);
  const result = binding.mkdir(path, mode, recursive);
  if (recursive && result !== undefined) {
    return result;
  }
}

function rmSync(path, options) {
  const opts = validateRmOptionsSync(path, options);
  path = getValidatedPath(path);
  binding.rmSync(path, opts.maxRetries, opts.recursive, opts.retryDelay);
}

function renameSync(oldPath, newPath) {
  const oldP = getValidatedPath(oldPath, 'oldPath');
  const newP = getValidatedPath(newPath, 'newPath');
  binding.rename(oldP, newP);
}

function rename(oldPath, newPath, callback) {
  callback = makeCallback(callback);
  let oldP;
  let newP;
  try {
    oldP = getValidatedPath(oldPath, 'oldPath');
    newP = getValidatedPath(newPath, 'newPath');
  } catch (e) {
    throw e;
  }
  setImmediateOrSync(() => {
    try {
      binding.rename(oldP, newP);
      callback(null);
    } catch (e) {
      callback(e);
    }
  });
}

function unlinkSync(path) {
  path = getValidatedPath(path);
  binding.unlink(path);
}

function unlink(path, callback) {
  callback = makeCallback(callback);
  let p;
  try {
    p = getValidatedPath(path);
  } catch (e) {
    throw e;
  }
  setImmediateOrSync(() => {
    try {
      binding.unlink(p);
      callback(null);
    } catch (e) {
      callback(e);
    }
  });
}

function rmdirSync(path, options) {
  path = getValidatedPath(path);
  if (options && options.recursive) {
    binding.rmSync(path, 0, true, 100);
    return;
  }
  binding.rmdir(path);
}

function truncateSync(path, len) {
  const length = len === undefined || len === null ? 0 : Number(len);
  const fd = openSync(path, 'r+');
  try {
    binding.ftruncate(fd, length);
  } finally {
    closeSync(fd);
  }
}

function ftruncateSync(fd, len) {
  if (typeof fd !== 'number' || Number.isNaN(fd)) {
    const err = new TypeError('The "fd" argument must be of type number. Received ' + String(fd));
    err.code = 'ERR_INVALID_ARG_TYPE';
    throw err;
  }
  const length = len === undefined || len === null ? 0 : Number(len);
  binding.ftruncate(fd, length);
}

// Valid copyfile flags: 0 or bitmask of COPYFILE_EXCL(1), FICLONE(2), FICLONE_FORCE(4) -> 0-7
function validateCopyFileMode(mode) {
  if (mode === undefined || mode === null) return 0;
  if (typeof mode !== 'number') {
    const err = new TypeError('The "mode" argument must be of type number. Received ' + (mode === null ? 'null' : typeof mode));
    err.code = 'ERR_INVALID_ARG_TYPE';
    throw err;
  }
  if (Number.isNaN(mode) || mode < 0 || mode > 7 || Math.floor(mode) !== mode) {
    const err = new RangeError('The value of "mode" is out of range. It must be >= 0 && <= 7. Received ' + mode);
    err.code = 'ERR_OUT_OF_RANGE';
    throw err;
  }
  return mode;
}

function copyFileSync(src, dest, mode) {
  const srcPath = getValidatedPath(src, 'src');
  const destPath = getValidatedPath(dest, 'dest');
  const flags = validateCopyFileMode(mode);
  binding.copyFile(srcPath, destPath, flags);
}

function copyFile(src, dest, flagsOrCallback, callback) {
  let flags = 0;
  if (typeof flagsOrCallback === 'function') {
    callback = flagsOrCallback;
    flags = 0;
  } else {
    flags = validateCopyFileMode(flagsOrCallback);
    if (typeof callback !== 'function') {
      const err = new TypeError('The "callback" argument must be of type Function. Received ' + (callback === null ? 'null' : typeof callback));
      err.code = 'ERR_INVALID_ARG_TYPE';
      throw err;
    }
  }
  callback = makeCallback(callback);
  const srcPath = getValidatedPath(src, 'src');
  const destPath = getValidatedPath(dest, 'dest');
  setImmediateOrSync(() => {
    try {
      binding.copyFile(srcPath, destPath, flags);
      callback(null);
    } catch (e) {
      callback(e);
    }
  });
}

function isBufferLike(value) {
  if (value == null || typeof value !== 'object') return false;
  if (typeof ArrayBuffer !== 'undefined' && typeof ArrayBuffer.isView === 'function' && ArrayBuffer.isView(value)) return true;
  return typeof value.byteLength === 'number' ||
    (value.constructor && (value.constructor.name === 'Buffer' || value.constructor.name === 'Uint8Array'));
}

function appendFileSync(pathOrFd, data, options) {
  options = getOptions(options, { encoding: 'utf8', mode: 0o666, flag: 'a' });
  const isFd = typeof pathOrFd === 'number' && !Number.isNaN(pathOrFd);
  const validData = typeof data === 'string' || isBufferLike(data);
  if (!validData) {
    const err = new TypeError('The "data" argument must be of type string or an instance of Buffer, TypedArray, or DataView. Received ' + (data === null ? 'null' : typeof data));
    err.code = 'ERR_INVALID_ARG_TYPE';
    throw err;
  }
  let fd;
  if (isFd) {
    fd = pathOrFd;
  } else {
    pathOrFd = getValidatedPath(pathOrFd);
    fd = openSync(pathOrFd, options.flag || 'a', options.mode);
  }
  try {
    if (typeof data === 'string' && (options.encoding === 'utf8' || options.encoding === 'utf-8')) {
      binding.writeSyncString(fd, data);
    } else if (isBufferLike(data)) {
      const len = data.byteLength ?? data.length;
      binding.writeSync(fd, data, 0, len, -1);
    } else {
      const err = new TypeError('The "data" argument must be of type string or an instance of Buffer, TypedArray, or DataView. Received ' + (data === null ? 'null' : typeof data));
      err.code = 'ERR_INVALID_ARG_TYPE';
      throw err;
    }
  } finally {
    if (!isFd) closeSync(fd);
  }
}

function readdir(path, options, callback) {
  if (typeof options === 'function') { callback = options; options = {}; }
  callback = makeCallback(callback);
  const p = getValidatedPath(path);
  setImmediateOrSync(() => {
    try {
      const withFileTypes = Boolean(options && options.withFileTypes);
      const result = binding.readdir(p, withFileTypes);
      if (withFileTypes && Array.isArray(result) && result.length === 2) {
        const names = result[0];
        const types = result[1];
        const out = [];
        for (let i = 0; i < names.length; i++) {
          out.push(new Dirent(names[i], types[i], path));
        }
        callback(null, out);
      } else {
        callback(null, result);
      }
    } catch (e) {
      callback(e);
    }
  });
}

function readdirSync(path, options) {
  options = getOptions(options, {});
  path = getValidatedPath(path);
  if (options.recursive) {
    throw new Error('readdirSync with recursive is not supported in this build');
  }
  const withFileTypes = Boolean(options.withFileTypes);
  const result = binding.readdir(path, withFileTypes);
  if (withFileTypes && Array.isArray(result) && result.length === 2) {
    const names = result[0];
    const types = result[1];
    const out = [];
    for (let i = 0; i < names.length; i++) {
      out.push(new Dirent(names[i], types[i], path));
    }
    return out;
  }
  return result;
}

function realpathSync(path, options) {
  path = getValidatedPath(path);
  return binding.realpath(path);
}
realpathSync.native = realpathSync;

function readlinkSync(path, options) {
  options = getOptions(options, { encoding: 'utf8' });
  path = getValidatedPath(path);
  return binding.readlink(path);
}

function readlink(path, options, callback) {
  if (typeof options === 'function') {
    callback = options;
    options = {};
  }
  callback = makeCallback(callback);
  path = getValidatedPath(path);
  setImmediateOrSync(() => {
    try {
      const linkString = binding.readlink(path);
      callback(null, linkString);
    } catch (e) {
      callback(e);
    }
  });
}

function symlinkTypeToFlags(type) {
  if (type === undefined || type === null || type === 'file') return 0;
  if (type === 'dir') return binding.UV_FS_SYMLINK_DIR;
  if (type === 'junction') return binding.UV_FS_SYMLINK_JUNCTION;
  const err = new TypeError('The "type" argument must be one of type string or undefined. Received \'' + type + '\'');
  err.code = 'ERR_INVALID_ARG_VALUE';
  throw err;
}

function symlinkSync(target, path, type) {
  const targetPath = getValidatedPath(target, 'target');
  const pathPath = getValidatedPath(path, 'path');
  const flags = symlinkTypeToFlags(type);
  binding.symlink(targetPath, pathPath, flags);
}

function symlink(target, path, type, callback) {
  if (typeof type === 'function') {
    callback = type;
    type = undefined;
  }
  callback = makeCallback(callback);
  const targetPath = getValidatedPath(target, 'target');
  const pathPath = getValidatedPath(path, 'path');
  const flags = symlinkTypeToFlags(type);
  setImmediateOrSync(() => {
    try {
      binding.symlink(targetPath, pathPath, flags);
      callback(null);
    } catch (e) {
      callback(e);
    }
  });
}

function chmodSync(path, mode) {
  path = getValidatedPath(path, 'path');
  const modeNum = typeof mode === 'number' ? mode : parseInt(String(mode), 8);
  if (Number.isNaN(modeNum)) {
    const err = new TypeError('The "mode" argument must be valid. Received ' + mode);
    err.code = 'ERR_INVALID_ARG_VALUE';
    throw err;
  }
  binding.chmod(path, modeNum);
}

function fchmodSync(fd, mode) {
  if (typeof fd !== 'number' || Number.isNaN(fd)) {
    const err = new TypeError('The "fd" argument must be of type number. Received ' + typeof fd);
    err.code = 'ERR_INVALID_ARG_TYPE';
    throw err;
  }
  const modeNum = typeof mode === 'number' ? mode : parseInt(String(mode), 8);
  if (Number.isNaN(modeNum)) {
    const err = new TypeError('The "mode" argument must be valid. Received ' + mode);
    err.code = 'ERR_INVALID_ARG_VALUE';
    throw err;
  }
  binding.fchmod(fd, modeNum);
}

function chmod(path, mode, callback) {
  if (typeof callback !== 'function') {
    const err = new TypeError('The "callback" argument must be of type Function. Received ' + typeof callback);
    err.code = 'ERR_INVALID_ARG_TYPE';
    throw err;
  }
  callback = makeCallback(callback);
  path = getValidatedPath(path, 'path');
  const modeNum = typeof mode === 'number' ? mode : parseInt(String(mode), 8);
  if (Number.isNaN(modeNum)) {
    const err = new TypeError('The "mode" argument must be valid. Received ' + mode);
    err.code = 'ERR_INVALID_ARG_VALUE';
    throw err;
  }
  setImmediateOrSync(() => {
    try {
      binding.chmod(path, modeNum);
      callback(null);
    } catch (e) {
      callback(e);
    }
  });
}

function utimes(path, atime, mtime, callback) {
  if (typeof callback !== 'function') {
    const err = new TypeError('The "callback" argument must be of type Function. Received ' + typeof callback);
    err.code = 'ERR_INVALID_ARG_TYPE';
    throw err;
  }
  callback = makeCallback(callback);
  path = getValidatedPath(path);
  setImmediateOrSync(() => {
    try {
      binding.utimes(path, toUnixTimestamp(atime), toUnixTimestamp(mtime));
      callback(null);
    } catch (e) {
      if (e && !e.code) e.code = 'ENOSYS';
      callback(e);
    }
  });
}

function fchmod(fd, mode, callback) {
  if (typeof callback !== 'function') {
    const err = new TypeError('The "callback" argument must be of type Function. Received ' + typeof callback);
    err.code = 'ERR_INVALID_ARG_TYPE';
    throw err;
  }
  callback = makeCallback(callback);
  if (typeof fd !== 'number' || Number.isNaN(fd)) {
    const err = new TypeError('The "fd" argument must be of type number. Received ' + typeof fd);
    err.code = 'ERR_INVALID_ARG_TYPE';
    throw err;
  }
  const modeNum = typeof mode === 'number' ? mode : parseInt(String(mode), 8);
  if (Number.isNaN(modeNum)) {
    const err = new TypeError('The "mode" argument must be valid. Received ' + mode);
    err.code = 'ERR_INVALID_ARG_VALUE';
    throw err;
  }
  setImmediateOrSync(() => {
    try {
      binding.fchmod(fd, modeNum);
      callback(null);
    } catch (e) {
      callback(e);
    }
  });
}

function toUnixTimestamp(time) {
  if (typeof time === 'number' && !Number.isNaN(time)) {
    if (time < 0) return Date.now() / 1000;
    return time;
  }
  if (time instanceof Date) return time.getTime() / 1000;
  if (typeof time === 'string' && +time === +time) return +time;
  throw new TypeError('atime or mtime must be a number or Date');
}

function utimesSync(path, atime, mtime) {
  path = getValidatedPath(path);
  binding.utimes(path, toUnixTimestamp(atime), toUnixTimestamp(mtime));
}

function futimesSync(fd, atime, mtime) {
  if (typeof fd !== 'number' || Number.isNaN(fd)) {
    const err = new TypeError('The "fd" argument must be of type number. Received ' + typeof fd);
    err.code = 'ERR_INVALID_ARG_TYPE';
    throw err;
  }
  if (fd < 0 || fd > 2147483647) {
    const err = new RangeError('The value of "fd" is out of range. It must be >= 0 && <= 2147483647. Received ' + fd);
    err.code = 'ERR_OUT_OF_RANGE';
    throw err;
  }
  binding.futimes(fd, toUnixTimestamp(atime), toUnixTimestamp(mtime));
}

function futimes(fd, atime, mtime, callback) {
  if (typeof callback !== 'function') {
    const err = new TypeError('The "callback" argument must be of type Function. Received ' + typeof callback);
    err.code = 'ERR_INVALID_ARG_TYPE';
    throw err;
  }
  callback = makeCallback(callback);
  if (typeof fd !== 'number' || Number.isNaN(fd)) {
    const err = new TypeError('The "fd" argument must be of type number. Received ' + typeof fd);
    err.code = 'ERR_INVALID_ARG_TYPE';
    throw err;
  }
  if (fd < 0 || fd > 2147483647) {
    const err = new RangeError('The value of "fd" is out of range. It must be >= 0 && <= 2147483647. Received ' + fd);
    err.code = 'ERR_OUT_OF_RANGE';
    throw err;
  }
  const at = toUnixTimestamp(atime);
  const mt = toUnixTimestamp(mtime);
  setImmediateOrSync(() => {
    try {
      binding.futimes(fd, at, mt);
      callback(null);
    } catch (e) {
      if (e && !e.code) e.code = 'ENOSYS';
      callback(e);
    }
  });
}

function lutimesSync(path, atime, mtime) {
  path = getValidatedPath(path);
  try {
    binding.lutimes(path, toUnixTimestamp(atime), toUnixTimestamp(mtime));
  } catch (e) {
    if (e) e.code = (e.code && e.code === 'ENOSYS') ? e.code : 'ENOSYS';
    throw e;
  }
}

function lutimes(path, atime, mtime, callback) {
  if (typeof callback !== 'function') {
    const err = new TypeError('The "callback" argument must be of type Function. Received ' + typeof callback);
    err.code = 'ERR_INVALID_ARG_TYPE';
    throw err;
  }
  callback = makeCallback(callback);
  path = getValidatedPath(path);
  const at = toUnixTimestamp(atime);
  const mt = toUnixTimestamp(mtime);
  setImmediateOrSync(() => {
    try {
      binding.lutimes(path, at, mt);
      callback(null);
    } catch (e) {
      if (e) e.code = (e.code && e.code === 'ENOSYS') ? e.code : 'ENOSYS';
      callback(e);
    }
  });
}

function fsyncSync(fd) {
  if (typeof fd !== 'number' || Number.isNaN(fd)) {
    const err = new TypeError('The "fd" argument must be of type number. Received ' + typeof fd);
    err.code = 'ERR_INVALID_ARG_TYPE';
    throw err;
  }
  binding.fsync(fd);
}

class FSWatcher extends EventEmitter {
  constructor() {
    super();
    this._closed = false;
  }

  close() {
    if (this._closed) return;
    this._closed = true;
    setImmediateOrSync(() => this.emit('close'));
  }
}

function watch(filename, options, listener) {
  if (typeof options === 'function') {
    listener = options;
    options = undefined;
  }
  if (typeof options === 'string') {
    options = { encoding: options };
  }
  options = options && typeof options === 'object' ? options : {};
  filename = getValidatedPath(filename, 'filename');
  void filename;

  const watcher = new FSWatcher();
  if (typeof listener === 'function') {
    watcher.on('change', listener);
  }

  const signal = options.signal;
  if (signal !== undefined) {
    const isAbortSignalLike =
      signal &&
      typeof signal === 'object' &&
      typeof signal.aborted === 'boolean' &&
      typeof signal.addEventListener === 'function' &&
      typeof signal.removeEventListener === 'function';
    if (!isAbortSignalLike) {
      const err = new TypeError(
        'The "options.signal" property must be an instance of AbortSignal.'
      );
      err.code = 'ERR_INVALID_ARG_TYPE';
      throw err;
    }

    if (signal.aborted) {
      watcher.close();
      return watcher;
    }

    const onAbort = () => watcher.close();
    signal.addEventListener('abort', onAbort, { once: true });
    watcher.once('close', () => signal.removeEventListener('abort', onAbort));
  }

  return watcher;
}

function mkdtempSync(prefix, options) {
  options = getOptions(options, { encoding: 'utf8' });
  prefix = getValidatedPath(prefix, 'prefix');
  prefix = String(prefix).normalize('NFC');
  return binding.mkdtemp(prefix);
}

function mkdtemp(prefix, options, callback) {
  if (typeof options === 'function') {
    callback = options;
    options = {};
  }
  callback = makeCallback(callback);
  options = getOptions(options, { encoding: 'utf8' });
  prefix = getValidatedPath(prefix, 'prefix');
  prefix = String(prefix).normalize('NFC');
  setImmediateOrSync(() => {
    try {
      const path = binding.mkdtemp(prefix);
      callback(null, path);
    } catch (e) {
      callback(e);
    }
  });
}

const constants = binding;

function Dir() {
  const err = new Error('Dir requires a path');
  err.code = 'ERR_MISSING_ARGS';
  throw err;
}

module.exports = {
  readFileSync,
  writeFileSync,
  mkdirSync,
  rmSync,
  readdir,
  readdirSync,
  realpathSync,
  readlinkSync,
  readlink,
  statSync,
  lstatSync,
  fstatSync,
  stat,
  lstat,
  fstat,
  existsSync,
  exists,
  accessSync,
  openSync,
  closeSync,
  readSync,
  writeSync,
  read,
  createReadStream,
  open,
  close,
  renameSync,
  unlink,
  unlinkSync,
  rmdirSync,
  truncateSync,
  ftruncateSync,
  copyFile,
  copyFileSync,
  appendFileSync,
  symlinkSync,
  symlink,
  chmodSync,
  chmod,
  fchmodSync,
  fchmod,
  utimesSync,
  utimes,
  futimesSync,
  futimes,
  lutimesSync,
  lutimes,
  fsyncSync,
  mkdtempSync,
  mkdtemp,
  watch,
  _toUnixTimestamp: toUnixTimestamp,
  constants,
  Dirent,
  Stats,
  Dir,
  FSWatcher,
};
module.exports.rename = rename;

function promisifyFs(fn) {
  return (...args) => new Promise((resolve, reject) => {
    fn(...args, (err, value) => {
      if (err) return reject(err);
      return resolve(value);
    });
  });
}

module.exports.promises = {
  symlink: promisifyFs(symlink),
  readlink: promisifyFs(readlink),
  stat: promisifyFs(stat),
  lstat: promisifyFs(lstat),
  utimes: promisifyFs(utimes),
  chmod: promisifyFs(chmod),
  unlink: promisifyFs(unlink),
  copyFile: promisifyFs(copyFile),
  mkdtemp: promisifyFs(mkdtemp),
  readdir: promisifyFs(readdir),
};
