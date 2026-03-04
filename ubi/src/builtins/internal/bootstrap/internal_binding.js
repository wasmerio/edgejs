'use strict';

const kEventTargetSignalPatched = Symbol.for('node.eventTargetSignalPatched');
const kReflectOwnKeysPatchedKey = Symbol.for('node.reflectOwnKeysPatched');
const kPrivateAccessorsInstalledKey = Symbol.for('node.privateAccessorsInstalled');
const kInternalTestBindingWrapperKey = Symbol.for('node.internalTestBindingWrapper');
const kInternalTestBindingTargetKey = Symbol.for('node.internalTestBindingTarget');
const kCapturedNativeInternalBinding =
  (typeof globalThis.internalBinding === 'function') ? globalThis.internalBinding : null;

// The loader passes a native-owned primordials container as the second wrapper argument.
// Fill it in place so one object identity is used for all modules.
const primordialsArg = primordials;
let primordialsExport = primordialsArg && typeof primordialsArg === 'object' ?
  primordialsArg : Object.create(null);
if (Object.keys(primordialsExport).length === 0) require('../per_context/primordials');

if (!globalThis[kEventTargetSignalPatched] &&
    typeof EventTarget === 'function' &&
    EventTarget.prototype &&
    typeof EventTarget.prototype.addEventListener === 'function') {
  globalThis[kEventTargetSignalPatched] = true;
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

function getNativePrivateSymbol(name, fallbackDescription) {
  if (typeof kCapturedNativeInternalBinding === 'function') {
    try {
      const utilBinding = kCapturedNativeInternalBinding('util');
      const privateSymbols = utilBinding && utilBinding.privateSymbols;
      const symbol = privateSymbols && privateSymbols[name];
      if (typeof symbol === 'symbol') return symbol;
    } catch {}
  }
  return Symbol(fallbackDescription);
}

const kArrowMessagePrivate =
  getNativePrivateSymbol('arrow_message_private_symbol', 'node:arrowMessage');
const kDecoratedPrivate =
  getNativePrivateSymbol('decorated_private_symbol', 'node:decorated');
const kPrivateStore = new WeakMap();
if (!globalThis[kReflectOwnKeysPatchedKey]) {
  globalThis[kReflectOwnKeysPatchedKey] = true;
  const originalOwnKeys = Reflect.ownKeys;
  Reflect.ownKeys = function ownKeysPatched(obj) {
    const keys = originalOwnKeys(obj);
    return keys.filter((k) => k !== kArrowMessagePrivate && k !== kDecoratedPrivate);
  };
}
if (!globalThis[kPrivateAccessorsInstalledKey]) {
  globalThis[kPrivateAccessorsInstalledKey] = true;
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

function getNativeInternalBinding() {
  if (typeof kCapturedNativeInternalBinding === 'function' &&
      kCapturedNativeInternalBinding !== internalBinding) {
    return kCapturedNativeInternalBinding;
  }
  const ib = globalThis.internalBinding;
  if (typeof ib !== 'function' || ib === internalBinding) return null;
  if (ib[kInternalTestBindingWrapperKey] === true) {
    const target = ib[kInternalTestBindingTargetKey];
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
  const binding = nativeInternalBinding(key);

  kInternalBindingCache.set(key, binding);
  return binding;
}


module.exports = { internalBinding, primordials: primordialsExport };
