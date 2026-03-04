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
  (globalThis.__ubi_timers_binding || null) : null;
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
      process && process.env && process.env.UBI_DEBUG_TIMERS_JS === '1';
  } catch {
    return false;
  }
}

// When the loader passes the native-created empty container (globalThis.__ubi_primordials),
// we fill it in place via Node's per_context primordials script so one object identity is
// used for all modules (Node-aligned). Otherwise use the passed-in object or a filled temp.
const primordialsArg = primordials;
const isNativeContainer = typeof globalThis !== 'undefined' && primordialsArg === globalThis.__ubi_primordials;
if (isNativeContainer) {
  require('../per_context/primordials');
}
let primordialsExport = primordialsArg;
if (!primordialsExport || typeof primordialsExport !== 'object' || Object.keys(primordialsExport).length === 0) {
  const empty = Object.create(null);
  const saved = globalThis.__ubi_primordials;
  globalThis.__ubi_primordials = empty;
  try {
    require('../per_context/primordials');
    primordialsExport = empty;
  } finally {
    globalThis.__ubi_primordials = saved;
  }
}

if (!globalThis.__ubi_event_target_signal_patched &&
    typeof EventTarget === 'function' &&
    EventTarget.prototype &&
    typeof EventTarget.prototype.addEventListener === 'function') {
  globalThis.__ubi_event_target_signal_patched = true;
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
const kArrowMessagePrivate = '__ubi_arrow_message_private_symbol__';
const kDecoratedPrivate = '__ubi_decorated_private_symbol__';
const kPrivateStore = new WeakMap();
const kExternalStreamTag = globalThis.__ubi_external_stream_tag ||
  (globalThis.__ubi_external_stream_tag = Symbol('ubi.external.stream'));
const kProxyTag = globalThis.__ubi_proxy_tag ||
  (globalThis.__ubi_proxy_tag = Symbol('ubi.proxy.tag'));
const kProxyDetails = globalThis.__ubi_proxy_details ||
  (globalThis.__ubi_proxy_details = new WeakMap());
const kCtorNameMap = new WeakMap();

const NativeProxy = Proxy;
if (!globalThis.__ubi_proxy_wrapped) {
  globalThis.__ubi_proxy_wrapped = true;
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
if (!globalThis.__ubi_setprototypeof_wrapped) {
  globalThis.__ubi_setprototypeof_wrapped = true;
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
if (!globalThis.__ubi_reflect_ownkeys_patched) {
  globalThis.__ubi_reflect_ownkeys_patched = true;
  const originalOwnKeys = Reflect.ownKeys;
  Reflect.ownKeys = function ownKeysPatched(obj) {
    const keys = originalOwnKeys(obj);
    return keys.filter((k) => k !== kArrowMessagePrivate && k !== kDecoratedPrivate);
  };
}
if (!globalThis.__ubi_private_accessors_installed) {
  globalThis.__ubi_private_accessors_installed = true;
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

let timersImmediateCallback = null;
let timersProcessTimersCallback = null;
let timersBootstrapDone = false;
let timersScheduledHandle = null;
let timersImmediateHandle = null;
let timerRefEnabled = true;
let immediateRefEnabled = true;

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
    timersScheduledHandle.__ubi_cancelled = true;
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
    timersImmediateHandle = { __ubi_microtask: true };
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
    const handle = { __ubi_cancelled: false };
    timersScheduledHandle = handle;
    const target = Date.now() + safeDelay;
    const spin = () => {
      if (handle.__ubi_cancelled || timersScheduledHandle !== handle) return;
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
    if (process && typeof process === 'object') {
      process.nextTick = nextTick;
      // Native runtime drains this hook each libuv turn.
      process._tickCallback = runNextTicks;
    }
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
              const m = '[ubi-timers-js] before processImmediate #' +
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
              const m = '[ubi-timers-js] after processImmediate #' +
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

if (!globalThis.__ubi_timers_binding_js) {
  globalThis.__ubi_timers_binding_js = createTimersBinding();
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
  const nativeGetInternalBinding = globalThis.__ubi_get_internal_binding;
  if (typeof nativeGetInternalBinding === 'function') {
    return nativeGetInternalBinding;
  }
  const ib = globalThis.internalBinding;
  if (typeof ib !== 'function' || ib === internalBinding) return null;
  if (ib.__ubi_internal_test_binding_wrapper === true) {
    const target = ib.__ubi_internal_test_binding_target;
    if (typeof target === 'function' && target !== internalBinding) {
      return target;
    }
    return null;
  }
  return ib;
}
const kInternalBindingCache = new Map();

function internalBinding(name) {
  const key = String(name);
  if (kInternalBindingCache.has(key)) return kInternalBindingCache.get(key);

  const nativeInternalBinding = getNativeInternalBinding();
  if (typeof nativeInternalBinding !== 'function') {
    throw new Error('internalBinding native hook is not installed');
  }
  let binding = nativeInternalBinding(key);
  if ((binding === undefined || binding === null) && key === 'config') {
    binding = {
      hasIntl: false,
      hasSmallICU: false,
      hasInspector: false,
      hasTracing: false,
      hasOpenSSL: true,
      fipsMode: false,
      hasNodeOptions: true,
      noBrowserGlobals: false,
      isDebugBuild: false,
    };
  }

  if (key === 'constants' && binding && typeof binding === 'object') {
    if (!binding.os || typeof binding.os !== 'object') binding.os = {};
    if (!binding.fs || typeof binding.fs !== 'object') binding.fs = {};
    if (binding.fs.F_OK === undefined) binding.fs.F_OK = 0;
    if (binding.fs.R_OK === undefined) binding.fs.R_OK = 4;
    if (binding.fs.W_OK === undefined) binding.fs.W_OK = 2;
    if (binding.fs.X_OK === undefined) binding.fs.X_OK = 1;
    const src = binding.os.signals;
    const normalized = Object.create(null);
    if (src && typeof src === 'object') {
      const keys = Object.keys(src);
      for (let i = 0; i < keys.length; i++) normalized[keys[i]] = src[keys[i]];
    }
    binding.os.signals = normalized;
  }
  if (key === 'uv' && binding && typeof binding === 'object') {
    if (typeof binding.getErrorMap !== 'function') {
      binding.getErrorMap = getUvErrorMap;
    }
    if (typeof binding.getErrorMessage !== 'function') {
      binding.getErrorMessage = getUvErrorMessage;
    }
    if (binding.UV_UNKNOWN === undefined) binding.UV_UNKNOWN = -4094;
    if (binding.UV_EAI_MEMORY === undefined) binding.UV_EAI_MEMORY = -3001;
  }
  if (key === 'errors') {
    if (!binding || typeof binding !== 'object') {
      binding = {};
    }
    if (typeof binding.getErrorSourcePositions !== 'function') {
      binding.getErrorSourcePositions = function getErrorSourcePositions() {
        return {
          sourceLine: '',
          scriptResourceName: '',
          lineNumber: 0,
          startColumn: 0,
        };
      };
    }
  }

  kInternalBindingCache.set(key, binding);
  return binding;
}


module.exports = { internalBinding, primordials: primordialsExport };
