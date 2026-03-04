'use strict';

const { AsyncResource } = require('internal/async_hooks');

const kAlsStores = Symbol.for('node.alsCurrentStores');
const kAlsTimersPatched = Symbol.for('node.alsTimersPatched');

let currentAlsStores = globalThis[kAlsStores];
if (!(currentAlsStores instanceof Map)) {
  currentAlsStores = new Map();
  globalThis[kAlsStores] = currentAlsStores;
}

function patchTimerForAls(name) {
  const original = globalThis[name];
  if (typeof original !== 'function') return;
  globalThis[name] = function patchedTimer(callback, ...args) {
    if (typeof callback !== 'function') {
      return original.call(this, callback, ...args);
    }
    const snapshot = new Map(currentAlsStores);
    const wrapped = function(...cbArgs) {
      const prev = currentAlsStores;
      currentAlsStores = new Map(snapshot);
      globalThis[kAlsStores] = currentAlsStores;
      try {
        return callback.apply(this, cbArgs);
      } finally {
        currentAlsStores = prev;
        globalThis[kAlsStores] = currentAlsStores;
      }
    };
    return original.call(this, wrapped, ...args);
  };
}

if (!globalThis[kAlsTimersPatched]) {
  patchTimerForAls('setImmediate');
  patchTimerForAls('setTimeout');
  patchTimerForAls('setInterval');
  globalThis[kAlsTimersPatched] = true;
}

class AsyncLocalStorage {
  run(store, callback, ...args) {
    if (typeof callback !== 'function') {
      throw new TypeError('callback must be a function');
    }
    const had = currentAlsStores.has(this);
    const prev = currentAlsStores.get(this);
    currentAlsStores.set(this, store);
    try {
      return callback(...args);
    } finally {
      if (had) currentAlsStores.set(this, prev);
      else currentAlsStores.delete(this);
    }
  }

  getStore() {
    return currentAlsStores.get(this);
  }
}

function createHook() {
  return {
    enable() { return this; },
    disable() { return this; },
  };
}

module.exports = {
  AsyncResource,
  AsyncLocalStorage,
  createHook,
  executionAsyncId() { return 0; },
  triggerAsyncId() { return 0; },
  executionAsyncResource() { return {}; },
};
