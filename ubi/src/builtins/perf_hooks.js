'use strict';

const kPerfObservers = Symbol.for('node.perfObservers');

class PerformanceObserverEntryList {
  constructor(entries) {
    this._entries = entries || [];
  }

  getEntries() {
    return this._entries.slice();
  }
}

class PerformanceObserver {
  constructor(callback) {
    this._callback = callback;
    this._types = new Set();
  }

  observe(options) {
    const type = options && options.type;
    if (!type) return;
    if (!globalThis[kPerfObservers]) globalThis[kPerfObservers] = new Map();
    if (!globalThis[kPerfObservers].has(type)) globalThis[kPerfObservers].set(type, new Set());
    globalThis[kPerfObservers].get(type).add(this);
    this._types.add(type);
  }

  disconnect() {
    if (!globalThis[kPerfObservers]) return;
    for (const type of this._types) {
      const set = globalThis[kPerfObservers].get(type);
      if (set) set.delete(this);
    }
    this._types.clear();
  }

  _push(entry) {
    if (typeof this._callback === 'function') {
      this._callback(new PerformanceObserverEntryList([entry]));
    }
  }
}

module.exports = {
  PerformanceObserver,
  performance: {
    now() {
      return Date.now();
    },
  },
};
