'use strict';

const UV_ENOENT = -2;
const UV_EEXIST = -17;
const kHasBackingStore = new WeakSet();
const basePrimordials = require('../util/primordials');

function uncurryThis(fn) {
  return Function.call.bind(fn);
}

const primordials = {
  ...basePrimordials,
  Array,
  ArrayBufferIsView: ArrayBuffer.isView,
  ArrayIsArray: Array.isArray,
  ArrayPrototypeForEach: uncurryThis(Array.prototype.forEach),
  BigInt,
  Float32Array,
  Float64Array,
  MathFloor: Math.floor,
  MathMin: Math.min,
  MathTrunc: Math.trunc,
  Number,
  NumberIsInteger: Number.isInteger,
  NumberIsNaN: Number.isNaN,
  NumberIsFinite: Number.isFinite,
  NumberMAX_SAFE_INTEGER: Number.MAX_SAFE_INTEGER,
  NumberMIN_SAFE_INTEGER: Number.MIN_SAFE_INTEGER,
  ObjectDefineProperties: Object.defineProperties,
  ObjectDefineProperty: Object.defineProperty,
  ObjectGetOwnPropertyDescriptor: Object.getOwnPropertyDescriptor,
  ObjectPrototypeHasOwnProperty: uncurryThis(Object.prototype.hasOwnProperty),
  ObjectSetPrototypeOf: Object.setPrototypeOf,
  RegExpPrototypeSymbolReplace: uncurryThis(RegExp.prototype[Symbol.replace]),
  StringPrototypeCharCodeAt: uncurryThis(String.prototype.charCodeAt),
  StringPrototypeSlice: uncurryThis(String.prototype.slice),
  StringPrototypeToLowerCase: uncurryThis(String.prototype.toLowerCase),
  StringPrototypeTrim: uncurryThis(String.prototype.trim),
  Symbol,
  SymbolFor: Symbol.for,
  SymbolSpecies: Symbol.species,
  SymbolToPrimitive: Symbol.toPrimitive,
  TypedArrayPrototypeFill: uncurryThis(Uint8Array.prototype.fill),
  TypedArrayPrototypeGetBuffer: (ta) => ta.buffer,
  TypedArrayPrototypeGetByteLength: (ta) => ta.byteLength,
  TypedArrayPrototypeGetByteOffset: (ta) => ta.byteOffset,
  TypedArrayPrototypeGetLength: (ta) => ta.length,
  TypedArrayPrototypeSet: uncurryThis(Uint8Array.prototype.set),
  TypedArrayPrototypeSubarray: uncurryThis(Uint8Array.prototype.subarray),
  TypedArrayPrototypeSlice: uncurryThis(Uint8Array.prototype.slice),
  Uint8Array,
  Uint8ArrayPrototype: Uint8Array.prototype,
};

const kUntransferable = Symbol('untransferable_object_private_symbol');
function getNativeInternalBinding() {
  const ib = globalThis.internalBinding;
  if (typeof ib === 'function' && ib !== internalBinding) return ib;
  return null;
}

function internalBinding(name) {
  if (name === 'uv') return { UV_ENOENT, UV_EEXIST };
  if (name === 'errors') {
    return {
      exitCodes: {
        kNoFailure: 0,
        kGenericUserError: 1,
      },
    };
  }
  if (name === 'os') return globalThis.__unode_os || {};
  if (name === 'buffer') return globalThis.__unode_buffer || {};
  if (name === 'string_decoder') {
    if (globalThis.__unode_string_decoder) return globalThis.__unode_string_decoder;
    const nativeInternalBinding = getNativeInternalBinding();
    if (nativeInternalBinding) return nativeInternalBinding(name);
    return {};
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
  if (name === 'util') {
    return {
      constants: {
        ALL_PROPERTIES: 0,
        ONLY_ENUMERABLE: 1,
      },
      getOwnNonIndexProperties(obj) {
        return Object.getOwnPropertyNames(obj).filter((n) => {
          const index = Number(n);
          return !Number.isInteger(index) || String(index) !== n;
        });
      },
      getCallSites() {
        return [];
      },
      isInsideNodeModules() {
        return false;
      },
      privateSymbols: {
        untransferable_object_private_symbol: kUntransferable,
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
  const nativeInternalBinding = getNativeInternalBinding();
  if (nativeInternalBinding) return nativeInternalBinding(name);
  return {};
}

module.exports = { internalBinding, primordials };
