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
const UV_ESRCH = -3;
const UV_UNKNOWN = -4094;
const UV_EAI_MEMORY = -3001;
const { getUvErrorMap, getUvErrorMessage } = require('../uv_errmap');
const kNativeTimersBinding = (typeof globalThis === 'object' && globalThis) ?
  (globalThis.__unode_timers_binding || null) : null;
const kHasBackingStore = new WeakSet();
const kImmediateInfo = (kNativeTimersBinding &&
  kNativeTimersBinding.immediateInfo instanceof Int32Array &&
  kNativeTimersBinding.immediateInfo.length >= 3) ?
  kNativeTimersBinding.immediateInfo : new Int32Array(3);
const kTimeoutInfo = (kNativeTimersBinding &&
  kNativeTimersBinding.timeoutInfo instanceof Int32Array &&
  kNativeTimersBinding.timeoutInfo.length >= 1) ?
  kNativeTimersBinding.timeoutInfo : new Int32Array(1);
const kTickInfo = new Int32Array(1);
const kIsBuildingSnapshotBuffer = new Uint8Array([0]);
const kSharedSymbolsBinding = (() => {
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
})();
let kDebugTimersImmediateCalls = 0;
function isDebugTimersJsEnabled() {
  try {
    return typeof process === 'object' &&
      process && process.env && process.env.UNODE_DEBUG_TIMERS_JS === '1';
  } catch {
    return false;
  }
}

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

if (!globalThis.__unode_event_target_signal_patched &&
    typeof EventTarget === 'function' &&
    EventTarget.prototype &&
    typeof EventTarget.prototype.addEventListener === 'function') {
  globalThis.__unode_event_target_signal_patched = true;
  const originalAddEventListener = EventTarget.prototype.addEventListener;
  const originalRemoveEventListener = EventTarget.prototype.removeEventListener;

  EventTarget.prototype.addEventListener = function addEventListener(type, listener, options) {
    if (options && typeof options === 'object' && Object.prototype.hasOwnProperty.call(options, 'signal')) {
      const signal = options.signal;
      if (signal !== undefined) {
        const isAbortSignalLike =
          signal &&
          typeof signal === 'object' &&
          typeof signal.aborted === 'boolean' &&
          typeof signal.addEventListener === 'function' &&
          typeof signal.removeEventListener === 'function';
        if (!isAbortSignalLike) {
          throw new TypeError('The "options.signal" property must be an instance of AbortSignal.');
        }
        if (signal.aborted) return;
        const capture = options.capture === true;
        const abortHandler = () => {
          try {
            originalRemoveEventListener.call(this, type, listener, capture);
          } catch {}
          try {
            signal.removeEventListener('abort', abortHandler);
          } catch {}
        };
        signal.addEventListener('abort', abortHandler, { once: true });
      }
    }
    return originalAddEventListener.call(this, type, listener, options);
  };
}

const kUntransferable = Symbol('untransferable_object_private_symbol');
const kArrowMessagePrivate = '__unode_arrow_message_private_symbol__';
const kDecoratedPrivate = '__unode_decorated_private_symbol__';
const kPrivateStore = new WeakMap();
const kExternalStreamTag = globalThis.__unode_external_stream_tag ||
  (globalThis.__unode_external_stream_tag = Symbol('unode.external.stream'));
const kProxyTag = globalThis.__unode_proxy_tag ||
  (globalThis.__unode_proxy_tag = Symbol('unode.proxy.tag'));
const kProxyDetails = globalThis.__unode_proxy_details ||
  (globalThis.__unode_proxy_details = new WeakMap());
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
let timersImmediateCallback = null;
let timersProcessTimersCallback = null;
let timersBootstrapDone = false;
let timersScheduledHandle = null;
let timersImmediateHandle = null;
let timerRefEnabled = true;
let immediateRefEnabled = true;
if (!globalThis.__unode_sigint_watchdog_listener_installed && typeof process?.on === 'function') {
  globalThis.__unode_sigint_watchdog_listener_installed = true;
  process.on('SIGINT', () => {
    if (watchdogDepth > 0) watchdogPending = true;
  });
}

function getNativeTimerPrimitives() {
  return {
    setTimeoutFn: typeof globalThis.setTimeout === 'function' ? globalThis.setTimeout.bind(globalThis) : null,
    clearTimeoutFn: typeof globalThis.clearTimeout === 'function' ? globalThis.clearTimeout.bind(globalThis) : null,
    setImmediateFn: typeof globalThis.setImmediate === 'function' ? globalThis.setImmediate.bind(globalThis) : null,
    clearImmediateFn: typeof globalThis.clearImmediate === 'function' ? globalThis.clearImmediate.bind(globalThis) : null,
    queueMicrotaskFn: typeof globalThis.queueMicrotask === 'function' ? globalThis.queueMicrotask.bind(globalThis) : null,
  };
}

function applyHandleRefState(handle, refEnabled) {
  if (!handle) return;
  if (refEnabled) {
    if (typeof handle.ref === 'function') handle.ref();
  } else if (typeof handle.unref === 'function') {
    handle.unref();
  }
}

function clearScheduledTimersHandle() {
  if (!timersScheduledHandle) return;
  if (timersScheduledHandle && typeof timersScheduledHandle === 'object') {
    timersScheduledHandle.__unode_cancelled = true;
  }
  const { clearTimeoutFn } = getNativeTimerPrimitives();
  if (clearTimeoutFn) {
    clearTimeoutFn(timersScheduledHandle);
  }
  timersScheduledHandle = null;
}

function clearScheduledImmediateHandle() {
  if (!timersImmediateHandle) return;
  const { clearImmediateFn, clearTimeoutFn } = getNativeTimerPrimitives();
  if (clearImmediateFn) {
    clearImmediateFn(timersImmediateHandle);
  } else if (clearTimeoutFn) {
    clearTimeoutFn(timersImmediateHandle);
  }
  timersImmediateHandle = null;
}

function scheduleImmediateDispatch() {
  if (timersImmediateHandle || !timersImmediateCallback) return;
  const dispatch = () => {
    timersImmediateHandle = null;
    if (typeof timersImmediateCallback !== 'function') return;
    timersImmediateCallback();
    if (kImmediateInfo[0] > 0) {
      scheduleImmediateDispatch();
    }
  };
  const { queueMicrotaskFn, setImmediateFn, setTimeoutFn } = getNativeTimerPrimitives();
  if (queueMicrotaskFn) {
    timersImmediateHandle = { __unode_microtask: true };
    queueMicrotaskFn(dispatch);
  } else if (setImmediateFn) {
    timersImmediateHandle = setImmediateFn(dispatch);
  } else if (setTimeoutFn) {
    timersImmediateHandle = setTimeoutFn(dispatch, 0);
  }
  applyHandleRefState(timersImmediateHandle, immediateRefEnabled);
}

function scheduleTimersDispatch(delayMs) {
  clearScheduledTimersHandle();
  const safeDelay = Number.isFinite(delayMs) ? Math.max(1, Math.trunc(delayMs)) : 1;
  const { queueMicrotaskFn, setTimeoutFn } = getNativeTimerPrimitives();
  if (queueMicrotaskFn) {
    const handle = { __unode_cancelled: false };
    timersScheduledHandle = handle;
    const target = Date.now() + safeDelay;
    const spin = () => {
      if (handle.__unode_cancelled || timersScheduledHandle !== handle) return;
      if (Date.now() >= target) {
        runTimersDispatch();
        return;
      }
      queueMicrotaskFn(spin);
    };
    queueMicrotaskFn(spin);
    return;
  }
  if (setTimeoutFn) {
    timersScheduledHandle = setTimeoutFn(runTimersDispatch, safeDelay);
    applyHandleRefState(timersScheduledHandle, timerRefEnabled);
  }
}

function runTimersDispatch() {
  timersScheduledHandle = null;
  if (typeof timersProcessTimersCallback !== 'function') return;
  const now = Date.now();
  const nextExpiry = Number(timersProcessTimersCallback(now));
  if (nextExpiry === 0 || !Number.isFinite(nextExpiry)) return;
  const absExpiry = Math.abs(nextExpiry);
  const delay = Math.max(1, absExpiry - Date.now());
  timerRefEnabled = nextExpiry > 0;
  scheduleTimersDispatch(delay);
}

function ensureTimersBootstrap() {
  if (timersBootstrapDone) return;
  timersBootstrapDone = true;
  try {
    const { setupTaskQueue } = require('internal/process/task_queues');
    const { nextTick, runNextTicks } = setupTaskQueue();
    if (process && typeof process === 'object') process.nextTick = nextTick;
    const { getTimerCallbacks } = require('internal/timers');
    const { processImmediate, processTimers } = getTimerCallbacks(runNextTicks);
    timersImmediateCallback = processImmediate;
    timersProcessTimersCallback = processTimers;
    const nativeTimers = kNativeTimersBinding;
    if (nativeTimers && typeof nativeTimers.setupTimers === 'function') {
      nativeTimers.setupTimers(timersImmediateCallback, timersProcessTimersCallback);
    }
  } catch (err) {
    timersBootstrapDone = false;
    throw err;
  }
}

function createTimersBinding() {
  const nativeTimers = kNativeTimersBinding;
  return {
    immediateInfo: kImmediateInfo,
    timeoutInfo: kTimeoutInfo,
    setupTimers(processImmediate, processTimers) {
      timersImmediateCallback = typeof processImmediate === 'function' ? processImmediate : null;
      timersProcessTimersCallback = typeof processTimers === 'function' ? processTimers : null;
      if (nativeTimers && typeof nativeTimers.setupTimers === 'function') {
        const wrappedImmediate = () => {
          kDebugTimersImmediateCalls++;
          if (isDebugTimersJsEnabled() &&
              (kDebugTimersImmediateCalls <= 20 || (kDebugTimersImmediateCalls % 1000) === 0)) {
            try {
              const m = '[unode-timers-js] before processImmediate #' +
                String(kDebugTimersImmediateCalls) +
                ' count=' + String(kImmediateInfo[0]) +
                ' refCount=' + String(kImmediateInfo[1]) +
                ' hasOutstanding=' + String(kImmediateInfo[2]);
              if (process && typeof process._rawDebug === 'function') process._rawDebug(m);
              else if (typeof console === 'object' && console && typeof console.error === 'function') console.error(m);
            } catch {}
          }
          if (kImmediateInfo[0] === 0 && kImmediateInfo[1] === 0 && kImmediateInfo[2] === 0) {
            if (typeof nativeTimers.toggleImmediateRef === 'function') {
              nativeTimers.toggleImmediateRef(false);
            }
            return;
          }
          if (typeof timersImmediateCallback === 'function') timersImmediateCallback();
          if (typeof nativeTimers.toggleImmediateRef === 'function') {
            nativeTimers.toggleImmediateRef(kImmediateInfo[1] > 0);
          }
          if (isDebugTimersJsEnabled() &&
              (kDebugTimersImmediateCalls <= 20 || (kDebugTimersImmediateCalls % 1000) === 0)) {
            try {
              const m = '[unode-timers-js] after processImmediate #' +
                String(kDebugTimersImmediateCalls) +
                ' count=' + String(kImmediateInfo[0]) +
                ' refCount=' + String(kImmediateInfo[1]) +
                ' hasOutstanding=' + String(kImmediateInfo[2]);
              if (process && typeof process._rawDebug === 'function') process._rawDebug(m);
              else if (typeof console === 'object' && console && typeof console.error === 'function') console.error(m);
            } catch {}
          }
        };
        nativeTimers.setupTimers(wrappedImmediate, timersProcessTimersCallback);
      } else if (kImmediateInfo[0] > 0) {
        scheduleImmediateDispatch();
      }
    },
    toggleTimerRef(ref) {
      ensureTimersBootstrap();
      timerRefEnabled = !!ref;
      if (nativeTimers && typeof nativeTimers.toggleTimerRef === 'function') {
        nativeTimers.toggleTimerRef(timerRefEnabled);
      } else {
        applyHandleRefState(timersScheduledHandle, timerRefEnabled);
      }
    },
    toggleImmediateRef(ref) {
      ensureTimersBootstrap();
      immediateRefEnabled = !!ref;
      if (nativeTimers && typeof nativeTimers.toggleImmediateRef === 'function') {
        nativeTimers.toggleImmediateRef(immediateRefEnabled);
      } else {
        applyHandleRefState(timersImmediateHandle, immediateRefEnabled);
        if (kImmediateInfo[0] > 0) scheduleImmediateDispatch();
      }
    },
    scheduleTimer(duration) {
      ensureTimersBootstrap();
      if (nativeTimers && typeof nativeTimers.scheduleTimer === 'function') {
        nativeTimers.scheduleTimer(duration);
      } else {
        scheduleTimersDispatch(duration);
      }
    },
    getLibuvNow() {
      if (nativeTimers && typeof nativeTimers.getLibuvNow === 'function') {
        return nativeTimers.getLibuvNow();
      }
      return Date.now();
    },
  };
}

if (!globalThis.__unode_timers_binding_js) {
  globalThis.__unode_timers_binding_js = createTimersBinding();
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
  const nativeGetInternalBinding = globalThis.__unode_get_internal_binding;
  if (typeof nativeGetInternalBinding === 'function') {
    return nativeGetInternalBinding;
  }
  const ib = globalThis.internalBinding;
  if (typeof ib !== 'function' || ib === internalBinding) return null;
  if (ib.__unode_internal_test_binding_wrapper === true) {
    const target = ib.__unode_internal_test_binding_target;
    if (typeof target === 'function' && target !== internalBinding) {
      return target;
    }
    return null;
  }
  return ib;
}
const kInternalBindingCache = new Map();
const kLazyGlobalBackedBindings = new Set([
  'buffer',
  'cares_wrap',
  'fs',
  'os',
  'pipe_wrap',
  'process_wrap',
  'signal_wrap',
  'spawn_sync',
  'stream_wrap',
  'tcp_wrap',
  'tty_wrap',
  'udp_wrap',
  'url',
  'util',
]);

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

function resolveFallbackBinding(name) {
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
      getErrorMap() {
        return getUvErrorMap();
      },
      getErrorMessage(err) {
        return getUvErrorMessage(err);
      },
    };
  }
  if (name === 'constants') {
    let fsConstants = {};
    let osConstants = {};
    try {
      const fs = require('fs');
      fsConstants = (fs && fs.constants) ? fs.constants : {};
    } catch {}
    try {
      const os = require('os');
      osConstants = (os && os.constants) ? os.constants : {};
    } catch {}
    return {
      os: {
        UV_UDP_REUSEADDR: 4,
        signals: (() => {
          const src = (osConstants && osConstants.signals) || {};
          const out = Object.create(null);
          const keys = Object.keys(src);
          for (let i = 0; i < keys.length; i++) out[keys[i]] = src[keys[i]];
          return out;
        })(),
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
    return globalThis.__unode_timers_binding_js || createTimersBinding();
  }
  if (name === 'task_queue') {
    return {
      tickInfo: kTickInfo,
      runMicrotasks() {},
      setTickCallback(fn) {
        if (process && typeof process === 'object' && typeof fn === 'function') {
          process._tickCallback = fn;
        }
      },
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
    return kSharedSymbolsBinding;
  }
  if (name === 'os') {
    const binding = globalThis.__unode_os || {};
    if (typeof binding.getCIDR !== 'function') {
      binding.getCIDR = () => null;
    }
    return binding;
  }
  if (name === 'buffer') return globalThis.__unode_buffer || {};
  if (name === 'crypto') {
    return globalThis.__unode_crypto_binding || globalThis.__unode_crypto || {};
  }
  if (name === 'http_parser') return globalThis.__unode_http_parser || {};
  if (name === 'stream_wrap') return globalThis.__unode_stream_wrap || {};
  if (name === 'process_wrap') {
    if (globalThis.__unode_process_wrap && typeof globalThis.__unode_process_wrap === 'object') {
      return globalThis.__unode_process_wrap;
    }
    class Process {
      constructor() {
        this.pid = 0;
        this._alive = false;
        this.onexit = null;
      }

      spawn(_options) {
        if (this._alive) return UV_EINVAL;
        let nextPid = typeof globalThis.__unode_process_wrap_pid === 'number' ?
          globalThis.__unode_process_wrap_pid : 30000;
        nextPid += 1;
        globalThis.__unode_process_wrap_pid = nextPid;
        this.pid = nextPid;
        this._alive = true;
        return 0;
      }

      kill(signal) {
        if (!this._alive) return UV_ESRCH;
        if (signal === 0) return 0;
        this._alive = false;
        const signalCode = signal == null ? 'SIGTERM' : signal;
        process.nextTick(() => {
          if (typeof this.onexit === 'function') this.onexit(0, signalCode);
        });
        return 0;
      }

      close() {
        this._alive = false;
      }

      ref() {}
      unref() {}
    }
    return { Process };
  }
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
  if (name === 'signal_wrap') {
    if (globalThis.__unode_signal_wrap && typeof globalThis.__unode_signal_wrap === 'object') {
      return globalThis.__unode_signal_wrap;
    }
    class UnodeSignalWrapStub {
      start() { return UV_EINVAL; }
      stop() { return 0; }
      close(cb) { if (typeof cb === 'function') cb(); }
      ref() {}
      unref() {}
      hasRef() { return false; }
      getAsyncId() { return -1; }
    }
    return { Signal: UnodeSignalWrapStub };
  }
  if (name === 'cares_wrap') {
    if (globalThis.__unode_cares_wrap) {
      const binding = globalThis.__unode_cares_wrap;
      if (typeof binding.getCIDR !== 'function') {
        binding.getCIDR = () => null;
      }
      return binding;
    }
    return {
      convertIpv6StringToBuffer() {
        return null;
      },
      getCIDR() {
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
          if (!scriptName || scriptName.includes('internal_binding.js')) continue;
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
    if (globalThis.__unode_tty_wrap && typeof globalThis.__unode_tty_wrap === 'object') {
      return globalThis.__unode_tty_wrap;
    }
    return {
      TTY: UnodeTTYWrap,
      isTTY(fd) {
        return Number.isInteger(fd) && fd >= 0 && fd <= 2;
      },
    };
  }
  if (name === 'spawn_sync') {
    const nativeSpawnSync = globalThis.__unode_spawn_sync;
    const normalizeResult = (raw) => {
      const result = raw && typeof raw === 'object' ? raw : {};
      const outputIn = Array.isArray(result.output) ? result.output : [null, '', ''];
      const toBuffer = (v) => {
        if (Buffer.isBuffer(v)) return v;
        if (typeof v === 'string') return Buffer.from(v, 'utf8');
        if (v && typeof v === 'object' && typeof v.byteLength === 'number') {
          try { return Buffer.from(v.buffer, v.byteOffset || 0, v.byteLength); } catch {}
        }
        return Buffer.alloc(0);
      };
      const output = [null, toBuffer(outputIn[1]), toBuffer(outputIn[2])];
      return {
        pid: typeof result.pid === 'number' ? result.pid : 0,
        output,
        status: result.status ?? null,
        signal: result.signal ?? null,
        error: result.error,
      };
    };
    if (nativeSpawnSync && typeof nativeSpawnSync.spawn === 'function') {
      return {
        spawn(options) {
          return normalizeResult(nativeSpawnSync.spawn(options));
        },
      };
    }
    return {
      spawn(options) {
        const cp = require('child_process');
        const file = options && options.file ? String(options.file) : '';
        const args = options && Array.isArray(options.args) ? options.args.slice(1).map((v) => String(v)) : [];
        const timeout = options && Number.isFinite(options.timeout) ? Number(options.timeout) : 0;
        const result = cp.spawnSync(file, args, { timeout });
        return normalizeResult({
          pid: typeof result.pid === 'number' ? result.pid : 0,
          output: [null, result.stdout || Buffer.alloc(0), result.stderr || Buffer.alloc(0)],
          status: result.status ?? null,
          signal: result.signal ?? null,
          error: result.error ? UV_EINVAL : undefined,
        });
      },
    };
  }
  return {};
}

function internalBinding(name) {
  const key = String(name);
  if (kInternalBindingCache.has(key)) return kInternalBindingCache.get(key);

  let binding;
  const nativeInternalBinding = getNativeInternalBinding();
  if (nativeInternalBinding) {
    try {
      binding = nativeInternalBinding(key);
    } catch {
      binding = undefined;
    }
  }
  if (binding === undefined || binding === null) {
    binding = resolveFallbackBinding(key);
  }

  if (key === 'constants' && binding && typeof binding === 'object') {
    if (!binding.os || typeof binding.os !== 'object') binding.os = {};
    const src = binding.os.signals;
    const normalized = Object.create(null);
    if (src && typeof src === 'object') {
      const keys = Object.keys(src);
      for (let i = 0; i < keys.length; i++) normalized[keys[i]] = src[keys[i]];
    }
    binding.os.signals = normalized;
  }

  if (kLazyGlobalBackedBindings.has(key) &&
      binding && typeof binding === 'object' &&
      Object.keys(binding).length === 0) {
    return binding;
  }
  kInternalBindingCache.set(key, binding);
  return binding;
}


module.exports = { internalBinding, primordials: primordialsExport };
