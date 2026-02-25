'use strict';

function uncurryThis(fn) {
  return Function.call.bind(fn);
}

const SymbolDispose = Symbol.dispose || Symbol.for('Symbol.dispose');

module.exports = {
  AsyncIteratorPrototype: Object.getPrototypeOf(
    Object.getPrototypeOf((async function* () {})())
  ),
  ArrayFrom: Array.from,
  ArrayPrototypeFilter: uncurryThis(Array.prototype.filter),
  ArrayPrototypeIndexOf: uncurryThis(Array.prototype.indexOf),
  ArrayPrototypeJoin: uncurryThis(Array.prototype.join),
  ArrayPrototypeSlice: uncurryThis(Array.prototype.slice),
  ArrayPrototypeSplice: uncurryThis(Array.prototype.splice),
  ArrayPrototypeMap: uncurryThis(Array.prototype.map),
  ArrayPrototypePop: uncurryThis(Array.prototype.pop),
  ArrayPrototypePush: uncurryThis(Array.prototype.push),
  ArrayPrototypeReverse: uncurryThis(Array.prototype.reverse),
  ArrayPrototypeShift: uncurryThis(Array.prototype.shift),
  ArrayPrototypeToSorted: Array.prototype.toSorted ?
    uncurryThis(Array.prototype.toSorted) :
    (arr, compareFn) => Array.from(arr).sort(compareFn),
  ArrayPrototypeUnshift: uncurryThis(Array.prototype.unshift),
  DateNow: Date.now,
  Boolean,
  Error,
  ErrorCaptureStackTrace: Error.captureStackTrace ?
    uncurryThis(Error.captureStackTrace) :
    ((target) => target),
  FunctionPrototypeBind: uncurryThis(Function.prototype.bind),
  FunctionPrototypeCall: uncurryThis(Function.prototype.call),
  MathCeil: Math.ceil,
  MathFloor: Math.floor,
  MathMax: Math.max,
  MathMaxApply: (arr) => Math.max.apply(Math, arr),
  NumberMAX_SAFE_INTEGER: Number.MAX_SAFE_INTEGER,
  NumberIsFinite: Number.isFinite,
  NumberIsNaN: Number.isNaN,
  ObjectDefineProperties: Object.defineProperties,
  ObjectDefineProperty: Object.defineProperty,
  ObjectGetPrototypeOf: Object.getPrototypeOf,
  ObjectSetPrototypeOf: Object.setPrototypeOf,
  Promise,
  PromiseReject: Promise.reject.bind(Promise),
  PromiseResolve: Promise.resolve.bind(Promise),
  ReflectApply: Reflect.apply,
  ReflectOwnKeys: Reflect.ownKeys,
  RegExpPrototypeExec: uncurryThis(RegExp.prototype.exec),
  SafeStringIterator: String,
  String,
  StringFromCharCode: String.fromCharCode,
  StringPrototypeCharCodeAt: uncurryThis(String.prototype.charCodeAt),
  StringPrototypeCodePointAt: uncurryThis(String.prototype.codePointAt),
  StringPrototypeEndsWith: uncurryThis(String.prototype.endsWith),
  StringPrototypeIncludes: uncurryThis(String.prototype.includes),
  StringPrototypeRepeat: uncurryThis(String.prototype.repeat),
  StringPrototypeReplaceAll: String.prototype.replaceAll ?
    uncurryThis(String.prototype.replaceAll) :
    (str, search, replacement) => String(str).split(search).join(replacement),
  StringPrototypeSlice: uncurryThis(String.prototype.slice),
  StringPrototypeSplit: uncurryThis(String.prototype.split),
  StringPrototypeStartsWith: uncurryThis(String.prototype.startsWith),
  StringPrototypeToLowerCase: uncurryThis(String.prototype.toLowerCase),
  Symbol,
  SymbolAsyncIterator: Symbol.asyncIterator,
  SymbolDispose,
  SymbolFor: Symbol.for,
};
