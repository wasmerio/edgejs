'use strict';

// Stub for Node's internal/test/binding used by raw Node tests (e.g. test-fs-copyfile.js).
// Libuv errno on Unix: UV_ERR(x) = -(x). ENOENT=2, EEXIST=17.
const UV_ENOENT = -2;
const UV_EEXIST = -17;
const kHasBackingStore = new WeakSet();

function uncurryThis(fn) {
  return Function.call.bind(fn);
}

const primordials = {
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
  ObjectPrototypeHasOwnProperty: uncurryThis(Object.prototype.hasOwnProperty),
  ObjectSetPrototypeOf: Object.setPrototypeOf,
  RegExpPrototypeSymbolReplace: uncurryThis(RegExp.prototype[Symbol.replace]),
  StringPrototypeCharCodeAt: uncurryThis(String.prototype.charCodeAt),
  StringPrototypeSlice: uncurryThis(String.prototype.slice),
  StringPrototypeToLowerCase: uncurryThis(String.prototype.toLowerCase),
  StringPrototypeTrim: uncurryThis(String.prototype.trim),
  SymbolSpecies: Symbol.species,
  SymbolToPrimitive: Symbol.toPrimitive,
  TypedArrayPrototypeFill: uncurryThis(Uint8Array.prototype.fill),
  TypedArrayPrototypeGetBuffer: (ta) => ta.buffer,
  TypedArrayPrototypeGetByteLength: (ta) => ta.byteLength,
  TypedArrayPrototypeGetByteOffset: (ta) => ta.byteOffset,
  TypedArrayPrototypeGetLength: (ta) => ta.length,
  TypedArrayPrototypeSet: uncurryThis(Uint8Array.prototype.set),
  TypedArrayPrototypeSlice: uncurryThis(Uint8Array.prototype.slice),
  Uint8Array,
  Uint8ArrayPrototype: Uint8Array.prototype,
};

const kUntransferable = Symbol('untransferable_object_private_symbol');

function internalBinding(name) {
  if (name === 'uv') {
    return {
      UV_ENOENT,
      UV_EEXIST,
    };
  }
  if (name === 'os') {
    return globalThis.__unode_os || {};
  }
  if (name === 'debug') {
    return {
      getV8FastApiCallCount() {
        return 0;
      },
    };
  }
  if (name === 'buffer') {
    return globalThis.__unode_buffer || {};
  }
  if (name === 'util') {
    return {
      constants: {
        ALL_PROPERTIES: 0,
        ONLY_ENUMERABLE: 1,
      },
      getOwnNonIndexProperties(obj) {
        return Object.getOwnPropertyNames(obj).filter((name) => {
          const index = Number(name);
          return !Number.isInteger(index) || String(index) !== name;
        });
      },
      isInsideNodeModules() {
        return false;
      },
      privateSymbols: {
        untransferable_object_private_symbol: kUntransferable,
      },
      arrayBufferViewHasBuffer(view) {
        if (view == null || typeof view !== 'object') {
          return false;
        }
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
  return {};
}

module.exports = { internalBinding, primordials };
