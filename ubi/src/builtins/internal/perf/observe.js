'use strict';

const active = new Map();
const kPerfObservers = Symbol.for('node.perfObservers');

function observersFor(type) {
  if (!globalThis[kPerfObservers]) globalThis[kPerfObservers] = new Map();
  if (!globalThis[kPerfObservers].has(type)) globalThis[kPerfObservers].set(type, new Set());
  return globalThis[kPerfObservers].get(type);
}

module.exports = {
  hasObserver(type) {
    return observersFor(type).size > 0;
  },
  startPerf(resource, symbol, data) {
    if (!resource || !symbol) return;
    active.set(resource, {
      symbol,
      startTime: Date.now(),
      ...(data || {}),
    });
  },
  stopPerf(resource, symbol, data) {
    const entry = active.get(resource);
    if (!entry || entry.symbol !== symbol) return;
    active.delete(resource);
    const now = Date.now();
    const perfEntry = {
      name: entry.name || data?.name || 'dns',
      entryType: entry.type || data?.type || 'dns',
      startTime: entry.startTime,
      duration: Math.max(0, now - entry.startTime),
      detail: { ...(entry.detail || {}), ...(data?.detail || {}) },
    };
    for (const obs of observersFor(perfEntry.entryType)) {
      try {
        obs._push(perfEntry);
      } catch {}
    }
  },
};
