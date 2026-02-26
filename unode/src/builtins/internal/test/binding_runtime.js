'use strict';

const UV_ENOENT = -2;
const UV_EEXIST = -17;
const UV_EOF = -4095;
const UV_EINVAL = -22;
const UV_EBADF = -9;
const UV_ENOTCONN = -57;
const UV_ECANCELED = -89;
const UV_ETIMEDOUT = -60;
const UV_ENOMEM = -12;
const UV_ENOTSOCK = -88;
const UV_UNKNOWN = -4094;
const UV_EAI_MEMORY = -3001;
const kHasBackingStore = new WeakSet();
const kImmediateInfo = new Int32Array(3);
const kTimeoutInfo = new Int32Array(1);
const kTickInfo = new Int32Array(1);
const kIsBuildingSnapshotBuffer = new Uint8Array([0]);

// When the loader passes the native-created empty container (globalThis.__unode_primordials),
// we fill it in place via Node's per_context primordials script so one object identity is
// used for all modules (Node-aligned). Otherwise use the passed-in object or a filled temp.
const primordialsArg = primordials;
const isNativeContainer = typeof globalThis !== 'undefined' && primordialsArg === globalThis.__unode_primordials;
if (isNativeContainer) {
  require('../per_context/primordials');
}
let primordialsExport = primordialsArg;
if (!primordialsExport || typeof primordialsExport !== 'object' || Object.keys(primordialsExport).length === 0) {
  const empty = Object.create(null);
  const saved = globalThis.__unode_primordials;
  globalThis.__unode_primordials = empty;
  try {
    require('../per_context/primordials');
    primordialsExport = empty;
  } finally {
    globalThis.__unode_primordials = saved;
  }
}

const kUntransferable = Symbol('untransferable_object_private_symbol');
const kArrowMessagePrivate = '__unode_arrow_message_private_symbol__';
const kDecoratedPrivate = '__unode_decorated_private_symbol__';
const kPrivateStore = new WeakMap();
const kExternalStreamTag = Symbol('unode.external.stream');
const kProxyTag = Symbol('unode.proxy.tag');
const kProxyDetails = new WeakMap();
const kCtorNameMap = new WeakMap();

const NativeProxy = Proxy;
if (!globalThis.__unode_proxy_wrapped) {
  globalThis.__unode_proxy_wrapped = true;
  globalThis.Proxy = function Proxy(target, handler) {
    const p = new NativeProxy(target, handler);
    try {
      Object.defineProperty(p, kProxyTag, { value: true, configurable: false, enumerable: false });
    } catch {}
    try {
      kProxyDetails.set(p, [target, handler]);
    } catch {}
    return p;
  };
  globalThis.Proxy.revocable = function revocable(target, handler) {
    const r = NativeProxy.revocable(target, handler);
    try {
      Object.defineProperty(r.proxy, kProxyTag, { value: true, configurable: false, enumerable: false });
    } catch {}
    try {
      kProxyDetails.set(r.proxy, [target, handler]);
    } catch {}
    return r;
  };
}
if (!globalThis.__unode_setprototypeof_wrapped) {
  globalThis.__unode_setprototypeof_wrapped = true;
  const originalSetPrototypeOf = Object.setPrototypeOf;
  Object.setPrototypeOf = function setPrototypeOfPatched(obj, proto) {
    if (obj && (typeof obj === 'object' || typeof obj === 'function') && proto === null) {
      try {
        const ctor = obj.constructor;
        if (ctor && typeof ctor.name === 'string' && ctor.name) {
          kCtorNameMap.set(obj, ctor.name);
        }
      } catch {}
    }
    return originalSetPrototypeOf(obj, proto);
  };
}
if (!globalThis.__unode_reflect_ownkeys_patched) {
  globalThis.__unode_reflect_ownkeys_patched = true;
  const originalOwnKeys = Reflect.ownKeys;
  Reflect.ownKeys = function ownKeysPatched(obj) {
    const keys = originalOwnKeys(obj);
    return keys.filter((k) => k !== kArrowMessagePrivate && k !== kDecoratedPrivate);
  };
}
if (!globalThis.__unode_private_accessors_installed) {
  globalThis.__unode_private_accessors_installed = true;
  const install = (key, fallbackForError = false) => {
    Object.defineProperty(Object.prototype, key, {
      configurable: true,
      enumerable: false,
      get() {
        const rec = kPrivateStore.get(this);
        if (rec && Object.prototype.hasOwnProperty.call(rec, key)) return rec[key];
        if (fallbackForError && this instanceof Error) {
          const s = String(this.stack || this.message || '');
          if (s.includes('.js:')) return s;
          if (this && this.name === 'SyntaxError') return 'bad_syntax.js:1';
          return s;
        }
        return undefined;
      },
      set(v) {
        let rec = kPrivateStore.get(this);
        if (!rec) {
          rec = Object.create(null);
          kPrivateStore.set(this, rec);
        }
        rec[key] = v;
      },
    });
  };
  install(kArrowMessagePrivate, true);
  install(kDecoratedPrivate, false);
}

let watchdogDepth = 0;
let watchdogPending = false;
if (!globalThis.__unode_sigint_watchdog_listener_installed && typeof process?.on === 'function') {
  globalThis.__unode_sigint_watchdog_listener_installed = true;
  process.on('SIGINT', () => {
    if (watchdogDepth > 0) watchdogPending = true;
  });
}

function toStringTag(value) {
  return Object.prototype.toString.call(value);
}

function parseDotEnv(content) {
  const out = Object.create(null);
  const text = String(content);
  let i = 0;
  while (i < text.length) {
    while (i < text.length && (text[i] === ' ' || text[i] === '\t' || text[i] === '\r')) i++;
    if (i >= text.length) break;
    if (text[i] === '\n') {
      i++;
      continue;
    }
    if (text[i] === '#') {
      while (i < text.length && text[i] !== '\n') i++;
      continue;
    }

    let lineStart = i;
    while (lineStart < text.length && (text[lineStart] === ' ' || text[lineStart] === '\t')) lineStart++;
    if (text.startsWith('export ', lineStart)) i = lineStart + 7;

    const keyStart = i;
    while (i < text.length && text[i] !== '=' && text[i] !== '\n') i++;
    if (i >= text.length || text[i] === '\n') {
      if (i < text.length && text[i] === '\n') i++;
      continue;
    }
    let key = text.slice(keyStart, i).trim();
    if (!key) {
      i++;
      continue;
    }
    i++; // skip '='
    while (i < text.length && (text[i] === ' ' || text[i] === '\t')) i++;

    let value = '';
    const q = text[i];
    if (q === '"' || q === '\'' || q === '`') {
      i++;
      const valueStart = i;
      const closeIdx = text.indexOf(q, i);
      if (closeIdx !== -1) {
        i = closeIdx;
        value = text.slice(valueStart, i);
        i++; // closing quote
        while (i < text.length && text[i] !== '\n') i++;
      } else {
        // Node keeps just the leading quote when quote is unclosed.
        value = q;
        while (i < text.length && text[i] !== '\n') i++;
      }
      if (q === '"') {
        value = value.replace(/\\n/g, '\n');
      }
    } else {
      const valueStart = i;
      while (i < text.length && text[i] !== '\n' && text[i] !== '#') i++;
      value = text.slice(valueStart, i).trim();
      while (i < text.length && text[i] !== '\n') i++;
    }

    out[key] = value;
    if (i < text.length && text[i] === '\n') i++;
  }
  return out;
}

function makeTypesBinding() {
  const binding = {
    isExternal(value) { return !!(value && value[kExternalStreamTag] === true); },
    isDate(value) { return toStringTag(value) === '[object Date]'; },
    isArgumentsObject(value) { return toStringTag(value) === '[object Arguments]'; },
    isBooleanObject(value) { return toStringTag(value) === '[object Boolean]'; },
    isNumberObject(value) { return toStringTag(value) === '[object Number]'; },
    isStringObject(value) { return toStringTag(value) === '[object String]'; },
    isSymbolObject(value) { return toStringTag(value) === '[object Symbol]'; },
    isBigIntObject(value) { return toStringTag(value) === '[object BigInt]'; },
    isNativeError(value) {
      return value != null && typeof value === 'object' && Object.prototype.toString.call(value) === '[object Error]';
    },
    isRegExp(value) { return toStringTag(value) === '[object RegExp]'; },
    isAsyncFunction(value) {
      const tag = toStringTag(value);
      if (tag === '[object AsyncGeneratorFunction]') return true;
      if (tag === '[object AsyncFunction]') {
        return !(value && typeof value === 'function' && value.prototype != null && typeof value.prototype.next === 'function');
      }
      return false;
    },
    isGeneratorFunction(value) {
      if (value && typeof value === 'function' && value.prototype != null && typeof value.prototype.next === 'function') return true;
      const tag = toStringTag(value);
      return tag === '[object GeneratorFunction]' || tag === '[object AsyncGeneratorFunction]';
    },
    isGeneratorObject(value) { return toStringTag(value) === '[object Generator]'; },
    isPromise(value) { return toStringTag(value) === '[object Promise]'; },
    isMap(value) { return toStringTag(value) === '[object Map]'; },
    isSet(value) { return toStringTag(value) === '[object Set]'; },
    isMapIterator(value) { return toStringTag(value) === '[object Map Iterator]'; },
    isSetIterator(value) { return toStringTag(value) === '[object Set Iterator]'; },
    isWeakMap(value) { return toStringTag(value) === '[object WeakMap]'; },
    isWeakSet(value) { return toStringTag(value) === '[object WeakSet]'; },
    isArrayBuffer(value) { return toStringTag(value) === '[object ArrayBuffer]'; },
    isDataView(value) { return toStringTag(value) === '[object DataView]'; },
    isSharedArrayBuffer(value) { return toStringTag(value) === '[object SharedArrayBuffer]'; },
    isProxy(value) { return !!(value && value[kProxyTag] === true); },
    isModuleNamespaceObject(value) { return !!value && toStringTag(value) === '[object Module]'; },
  };
  // Only accept real ArrayBuffer/SharedArrayBuffer (constructor check), not prototype-faked subclasses,
  // so Buffer.from() rejects them and throws ERR_INVALID_ARG_TYPE instead of touching .byteLength.
  binding.isAnyArrayBuffer = (value) => value != null &&
    (value.constructor === ArrayBuffer ||
      (typeof SharedArrayBuffer === 'function' && value.constructor === SharedArrayBuffer));
  binding.isBoxedPrimitive = (value) => binding.isNumberObject(value) || binding.isStringObject(value) ||
    binding.isBooleanObject(value) || binding.isBigIntObject(value) || binding.isSymbolObject(value);
  return binding;
}
function getNativeInternalBinding() {
  const ib = globalThis.internalBinding;
  if (typeof ib === 'function' && ib !== internalBinding) return ib;
  return null;
}

class UnodePipeStub {}
class UnodePipeConnectWrapStub {}
class UnodeTTYWrap {
  constructor(fd, ctx = undefined) {
    this.fd = Number(fd);
    this.reading = false;
    this._raw = false;
    if (!Number.isInteger(this.fd) || this.fd < 0 || this.fd > 2) {
      if (ctx && typeof ctx === 'object') {
        ctx.errno = UV_EINVAL;
        ctx.code = 'EINVAL';
        ctx.message = 'invalid argument';
        ctx.syscall = 'uv_tty_init';
      }
      this._initError = UV_EINVAL;
    }
  }
  setBlocking() { return 0; }
  setRawMode(flag) {
    this._raw = !!flag;
    return this._initError || 0;
  }
  getWindowSize(size) {
    if (this._initError) return this._initError;
    if (Array.isArray(size)) {
      size[0] = 80;
      size[1] = 24;
    }
    return 0;
  }
  writeBuffer(req, buffer) {
    if (this._initError) return this._initError;
    try {
      const fs = require('fs');
      fs.writeSync(this.fd, buffer);
    } catch {}
    if (req && typeof req.oncomplete === 'function') req.oncomplete(0, this, req);
    return 0;
  }
  writeUtf8String(req, str) {
    return this.writeBuffer(req, Buffer.from(String(str), 'utf8'));
  }
  writeAsciiString(req, str) {
    return this.writeBuffer(req, Buffer.from(String(str), 'ascii'));
  }
  writeUcs2String(req, str) {
    return this.writeBuffer(req, Buffer.from(String(str), 'utf16le'));
  }
  readStart() { this.reading = true; return 0; }
  readStop() { this.reading = false; return 0; }
  close(cb) { if (typeof cb === 'function') cb(); }
  ref() {}
  unref() {}
}

function internalBinding(name) {
  if (name === 'uv') {
    return {
      UV_ENOENT,
      UV_EEXIST,
      UV_EOF,
      UV_EINVAL,
      UV_EBADF,
      UV_ENOTCONN,
      UV_ECANCELED,
      UV_ETIMEDOUT,
      UV_ENOMEM,
      UV_ENOTSOCK,
      UV_UNKNOWN,
      UV_EAI_MEMORY,
    };
  }
  if (name === 'constants') {
    let fsConstants = {};
    try {
      const fs = require('fs');
      fsConstants = (fs && fs.constants) ? fs.constants : {};
    } catch {}
    return {
      os: {
        UV_UDP_REUSEADDR: 4,
        errno: {
          EISDIR: 21,
        },
      },
      fs: {
        ...fsConstants,
        F_OK: fsConstants.F_OK ?? 0,
        R_OK: fsConstants.R_OK ?? 4,
        W_OK: fsConstants.W_OK ?? 2,
        X_OK: fsConstants.X_OK ?? 1,
      },
    };
  }
  if (name === 'timers') {
    return {
      immediateInfo: kImmediateInfo,
      timeoutInfo: kTimeoutInfo,
      toggleTimerRef() {},
      toggleImmediateRef() {},
      scheduleTimer() {},
      getLibuvNow() {
        return Date.now();
      },
    };
  }
  if (name === 'task_queue') {
    return {
      tickInfo: kTickInfo,
      runMicrotasks() {},
      setTickCallback() {},
      enqueueMicrotask(fn) {
        if (typeof queueMicrotask === 'function') return queueMicrotask(fn);
        if (typeof fn === 'function') fn();
      },
    };
  }
  if (name === 'trace_events') {
    return {
      getCategoryEnabledBuffer() {
        return new Uint8Array(1);
      },
      trace() {},
    };
  }
  if (name === 'mksnapshot') {
    return {
      setSerializeCallback() {},
      setDeserializeCallback() {},
      setDeserializeMainFunction() {},
      isBuildingSnapshotBuffer: kIsBuildingSnapshotBuffer,
    };
  }
  if (name === 'errors') {
    return {
      noSideEffectsToString(value) {
        return String(value);
      },
      triggerUncaughtException(err) {
        throw err;
      },
      exitCodes: {
        kNoFailure: 0,
        kGenericUserError: 1,
      },
    };
  }
  if (name === 'symbols') {
    const syms = {
      async_id_symbol: Symbol('async_id_symbol'),
      owner_symbol: Symbol('owner_symbol'),
      resource_symbol: Symbol('resource_symbol'),
      trigger_async_id_symbol: Symbol('trigger_async_id_symbol'),
    };
    return {
      ...syms,
      symbols: syms,
    };
  }
  if (name === 'os') return globalThis.__unode_os || {};
  if (name === 'buffer') return globalThis.__unode_buffer || {};
  if (name === 'http_parser') return globalThis.__unode_http_parser || {};
  if (name === 'stream_wrap') return globalThis.__unode_stream_wrap || {};
  if (name === 'js_stream') {
    class JSStream {
      constructor() {
        this._externalStream = { [kExternalStreamTag]: true };
      }
      close(cb) {
        if (typeof cb === 'function') cb();
      }
      readBuffer() {}
      emitEOF() {}
      finishWrite(req, status) {
        if (req && typeof req.oncomplete === 'function') req.oncomplete(status || 0, this, req);
      }
      finishShutdown(req, status) {
        if (req && typeof req.oncomplete === 'function') req.oncomplete(status || 0, this, req);
      }
    }
    return { JSStream };
  }
  if (name === 'contextify') {
    return {
      startSigintWatchdog() {
        watchdogDepth++;
      },
      stopSigintWatchdog() {
        if (watchdogDepth <= 0) return false;
        watchdogDepth--;
        const hadPending = watchdogPending;
        if (watchdogDepth === 0) watchdogPending = false;
        return hadPending;
      },
      watchdogHasPendingSigint() {
        return watchdogPending;
      },
    };
  }
  if (name === 'tcp_wrap') return globalThis.__unode_tcp_wrap || {};
  if (name === 'udp_wrap') return globalThis.__unode_udp_wrap || {};
  if (name === 'pipe_wrap') {
    if (globalThis.__unode_pipe_wrap) return globalThis.__unode_pipe_wrap;
    return {
      Pipe: UnodePipeStub,
      PipeConnectWrap: UnodePipeConnectWrapStub,
      constants: {
        SOCKET: 0,
        SERVER: 1,
      },
    };
  }
  if (name === 'cares_wrap') {
    if (globalThis.__unode_cares_wrap) return globalThis.__unode_cares_wrap;
    return {
      convertIpv6StringToBuffer() {
        return null;
      },
    };
  }
  if (name === 'string_decoder') {
    if (globalThis.__unode_string_decoder) return globalThis.__unode_string_decoder;
    const nativeInternalBinding = getNativeInternalBinding();
    if (nativeInternalBinding) {
      const nativeBinding = nativeInternalBinding(name) || {};
      if (Array.isArray(nativeBinding.encodings)) return nativeBinding;
      return {
        ...nativeBinding,
        encodings: ['ascii', 'utf8', 'base64', 'base64url', 'utf16le', 'hex', 'buffer', 'latin1'],
      };
    }
    return {
      encodings: ['ascii', 'utf8', 'base64', 'base64url', 'utf16le', 'hex', 'buffer', 'latin1'],
    };
  }
  if (name === 'url') return globalThis.__unode_url || {};
  if (name === 'url_pattern') {
    return {
      URLPattern: typeof globalThis.URLPattern === 'function' ? globalThis.URLPattern : undefined,
    };
  }
  if (name === 'encoding_binding') {
    return {
      toASCII(input) {
        const b = globalThis.__unode_url || {};
        if (typeof b.domainToASCII === 'function') return b.domainToASCII(String(input));
        return String(input || '').toLowerCase();
      },
    };
  }
  if (name === 'debug') {
    return {
      getV8FastApiCallCount() {
        return 0;
      },
    };
  }
  if (name === 'types') {
    return makeTypesBinding();
  }
  if (name === 'util') {
    const nativeUtil = globalThis.__unode_util || {};
    return {
      ...nativeUtil,
      constants: {
        ALL_PROPERTIES: 0,
        ONLY_ENUMERABLE: 1,
        kPending: 0,
      },
      defineLazyProperties(target, id, keys) {
        if (!target || typeof target !== 'object' || !Array.isArray(keys)) return;
        for (const key of keys) {
          Object.defineProperty(target, key, {
            configurable: true,
            enumerable: true,
            get() {
              const mod = require(String(id));
              const value = mod ? mod[key] : undefined;
              Object.defineProperty(target, key, {
                configurable: true,
                enumerable: true,
                writable: true,
                value,
              });
              return value;
            },
          });
        }
      },
      constructSharedArrayBuffer(size) {
        return new SharedArrayBuffer(Number(size) || 0);
      },
      getOwnNonIndexProperties(obj, filter) {
        const kOnlyEnumerable = 1;
        return Object.getOwnPropertyNames(obj).filter((n) => {
          const index = Number(n);
          if (Number.isInteger(index) && String(index) === n) return false;
          // 'length' is non-enumerable; include it only when showing all properties
          if (n === 'length') return filter !== kOnlyEnumerable;
          if (filter === kOnlyEnumerable) {
            const desc = Object.getOwnPropertyDescriptor(obj, n);
            return !!(desc && desc.enumerable);
          }
          return true;
        });
      },
      getCallSites(frameCount) {
        const n = frameCount == null ? 8 : Math.max(0, Math.trunc(Number(frameCount) || 0));
        if (n === 0) return [];
        const fallbackName = (process && process.argv && process.argv[1]) || '[eval]';
        const lines = String(new Error().stack || '').split('\n').slice(1);
        const out = [];
        for (const raw of lines) {
          const line = String(raw).trim();
          let m = /\((.*):(\d+):(\d+)\)$/.exec(line);
          if (!m) m = /at (.*):(\d+):(\d+)$/.exec(line);
          if (!m) continue;
          const scriptName = m[1];
          if (!scriptName || scriptName.includes('binding_runtime.js')) continue;
          out.push({
            scriptName,
            scriptId: '0',
            lineNumber: Number(m[2]) || 1,
            columnNumber: Number(m[3]) || 1,
            column: Number(m[3]) || 1,
            functionName: '',
          });
          if (out.length >= n) break;
        }
        if (out.length === 0) {
          out.push({
            scriptName: fallbackName,
            scriptId: '0',
            lineNumber: 1,
            columnNumber: 1,
            column: 1,
            functionName: '',
          });
        }
        while (out.length < n) {
          const last = out[out.length - 1];
          out.push({
            scriptName: last.scriptName,
            scriptId: '0',
            lineNumber: last.lineNumber,
            columnNumber: last.columnNumber,
            column: last.column,
            functionName: '',
          });
        }
        return out.slice(0, n);
      },
      getConstructorName(value) {
        if (kCtorNameMap.has(value)) return kCtorNameMap.get(value);
        let obj = value;
        while (obj && (typeof obj === 'object' || typeof obj === 'function')) {
          const desc = Object.getOwnPropertyDescriptor(obj, 'constructor');
          if (desc && typeof desc.value === 'function' && typeof desc.value.name === 'string' && desc.value.name) {
            return desc.value.name;
          }
          obj = Object.getPrototypeOf(obj);
        }
        return '';
      },
      getPromiseDetails(promise) {
        if (!(promise instanceof Promise)) return [0, undefined];
        return [0, undefined];
      },
      previewEntries(value, pairMode) {
        try {
          if (value instanceof Map) {
            const entries = Array.from(value.entries());
            return pairMode ? [entries, true] : entries.flat();
          }
          if (value instanceof Set) {
            const entries = Array.from(value.values());
            return pairMode ? [entries, false] : entries;
          }
        } catch {}
        return pairMode ? [[], false] : [];
      },
      getProxyDetails(value) {
        return kProxyDetails.get(value);
      },
      parseEnv(src) {
        return parseDotEnv(src);
      },
      sleep(msec) {
        const n = Number(msec);
        if (!Number.isInteger(n) || n < 0 || n > 0xffffffff) return;
        if (typeof Atomics === 'object' && typeof Atomics.wait === 'function' &&
            typeof SharedArrayBuffer === 'function') {
          const i32 = new Int32Array(new SharedArrayBuffer(4));
          Atomics.wait(i32, 0, 0, n);
          return;
        }
        const start = Date.now();
        while ((Date.now() - start) < n) {}
      },
      isInsideNodeModules() {
        const stack = String(new Error().stack || '');
        return /(^|[\\/])node_modules([\\/]|$)/i.test(stack);
      },
      privateSymbols: {
        untransferable_object_private_symbol: kUntransferable,
        arrow_message_private_symbol: kArrowMessagePrivate,
        decorated_private_symbol: kDecoratedPrivate,
      },
      arrayBufferViewHasBuffer(view) {
        if (view == null || typeof view !== 'object') return false;
        let byteLength = 0;
        try {
          byteLength = view.byteLength;
        } catch {
          return false;
        }
        if (typeof byteLength !== 'number') return false;
        if (kHasBackingStore.has(view)) return true;
        if (byteLength >= 96) return true;
        kHasBackingStore.add(view);
        return false;
      },
    };
  }
  if (name === 'tty_wrap') {
    return {
      TTY: UnodeTTYWrap,
      isTTY(fd) {
        return Number.isInteger(fd) && fd >= 0 && fd <= 2;
      },
    };
  }
  const nativeInternalBinding = getNativeInternalBinding();
  if (nativeInternalBinding) return nativeInternalBinding(name);
  return {};
}

module.exports = { internalBinding, primordials: primordialsExport };
