'use strict';

const binding = globalThis.__unode_fs;
if (!binding) {
  throw new Error('fs builtin requires __unode_fs binding');
}

function pathTypeError(path) {
  const err = new TypeError('path must be a string, Buffer, or URL. Received ' + (path === null ? 'null' : typeof path));
  err.code = 'ERR_INVALID_ARG_TYPE';
  return err;
}

function getValidatedPath(path) {
  if (path == null) {
    throw pathTypeError(path);
  }
  if (typeof path === 'object' && path && path.href !== undefined) {
    path = path.pathname || path.href;
  } else if (typeof path === 'object' && path) {
    throw pathTypeError(path);
  }
  if (typeof path !== 'string' && typeof path === 'object' && path && path.toString) {
    path = path.toString();
  }
  if (typeof path !== 'string') {
    throw pathTypeError(path);
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
  const setImmediate = globalThis.setImmediate || function (fn) { fn(); };
  setImmediate(() => {
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
  setImmediateOrSync(() => {
    try {
      const p = getValidatedPath(path);
      const f = flags == null ? binding.O_RDONLY : (typeof flags === 'string' ? stringToFlags(flags) : flags);
      const m = mode === undefined || mode === null ? 0o666 : parseFileMode(mode, 'mode', 0o666);
      const fd = binding.open(p, f, m);
      callback(null, fd);
    } catch (e) {
      callback(e);
    }
  });
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
    const buf = new Uint8Array(8192);
    let n;
    try {
      while ((n = binding.readSync(fd, buf, 0, buf.length, -1)) > 0) {
        chunks.push(buf.slice(0, n));
      }
    } finally {
      closeSync(fd);
    }
    const total = chunks.reduce((acc, c) => acc + c.length, 0);
    const out = new Uint8Array(total);
    let off = 0;
    for (const c of chunks) {
      out.set(c, off);
      off += c.length;
    }
    out.equals = function (b) {
      if (this === b) return true;
      const other = b && (typeof b.length === 'number' && b.byteLength !== undefined)
        ? b
        : (b && typeof b === 'object' && b.length !== undefined ? new Uint8Array(b) : null);
      if (!other || this.length !== other.length) return false;
      for (let i = 0; i < this.length; i++) {
        if (this[i] !== other[i]) return false;
      }
      return true;
    };
    return out;
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
  const oldP = getValidatedPath(oldPath);
  const newP = getValidatedPath(newPath);
  binding.rename(oldP, newP);
}

function unlinkSync(path) {
  path = getValidatedPath(path);
  binding.unlink(path);
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

function copyFileSync(src, dest, mode) {
  const srcPath = getValidatedPath(src);
  const destPath = getValidatedPath(dest);
  const flags = mode === undefined || mode === null ? 0 : mode;
  binding.copyFile(srcPath, destPath, flags);
}

function isBufferLike(value) {
  if (value == null || typeof value !== 'object') return false;
  if (typeof ArrayBuffer !== 'undefined' && typeof ArrayBuffer.isView === 'function' && ArrayBuffer.isView(value)) return true;
  return typeof value.byteLength === 'number' ||
    (value.constructor && (value.constructor.name === 'Buffer' || value.constructor.name === 'Uint8Array'));
}

function appendFileSync(path, data, options) {
  options = getOptions(options, { encoding: 'utf8', mode: 0o666, flag: 'a' });
  path = getValidatedPath(path);
  const fd = openSync(path, options.flag || 'a', options.mode);
  try {
    if (typeof data === 'string' && (options.encoding === 'utf8' || options.encoding === 'utf-8')) {
      binding.writeSyncString(fd, data);
    } else if (isBufferLike(data)) {
      const len = data.byteLength ?? data.length;
      binding.writeSync(fd, data, 0, len, -1);
    } else if (data != null && typeof data === 'object' && typeof data.length === 'number' && data.length >= 0) {
      const u8 = new Uint8Array(data.length);
      for (let i = 0; i < data.length; i++) u8[i] = data[i];
      binding.writeSync(fd, u8, 0, u8.length, -1);
    } else {
      const err = new TypeError('The "data" argument must be of type string or an instance of Buffer, TypedArray, or DataView. Received ' + (data === null ? 'null' : typeof data));
      err.code = 'ERR_INVALID_ARG_TYPE';
      throw err;
    }
  } finally {
    closeSync(fd);
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
  open,
  close,
  renameSync,
  unlinkSync,
  rmdirSync,
  truncateSync,
  ftruncateSync,
  copyFileSync,
  appendFileSync,
  constants,
  Dirent,
  Stats,
  Dir,
};
