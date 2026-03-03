'use strict';

// Ensure Event/EventTarget/CustomEvent globals exist before loading events tests.
require('internal/event_target');

const exported = require('../../../node-lib/events.js');

// Provide a lightweight process event emitter surface in runtimes where process
// does not expose Node's full EventEmitter behavior.
if (typeof process === 'object' && process != null && typeof process.emit !== 'function') {
  const listeners = new Map();
  process.on = process.addListener = function on(name, listener) {
    const key = String(name);
    const arr = listeners.get(key) || [];
    arr.push(listener);
    listeners.set(key, arr);
    return process;
  };
  process.removeListener = function removeListener(name, listener) {
    const key = String(name);
    const arr = listeners.get(key);
    if (!arr) return process;
    const idx = arr.lastIndexOf(listener);
    if (idx >= 0) arr.splice(idx, 1);
    listeners.set(key, arr);
    return process;
  };
  process.once = function once(name, listener) {
    function wrapped(...args) {
      process.removeListener(name, wrapped);
      return listener.apply(this, args);
    }
    return process.on(name, wrapped);
  };
  process.emit = function emit(name, ...args) {
    const arr = (listeners.get(String(name)) || []).slice();
    for (const fn of arr) {
      fn.apply(process, args);
    }
    return arr.length > 0;
  };
  process.removeAllListeners = function removeAllListeners(name) {
    if (name === undefined) {
      listeners.clear();
    } else {
      listeners.delete(String(name));
    }
    return process;
  };
  process.listenerCount = function listenerCount(name) {
    const arr = listeners.get(String(name));
    return arr ? arr.length : 0;
  };
}

// Node marks AbortSignal max listeners as 0 by default.
if (typeof AbortSignal === 'function' &&
    AbortSignal.prototype &&
    exported &&
    exported.kMaxEventTargetListeners) {
  try {
    AbortSignal.prototype[exported.kMaxEventTargetListeners] = 0;
  } catch {
    // Ignore if prototype is not writable in this runtime.
  }
}
if (typeof EventTarget === 'function' &&
    EventTarget.prototype &&
    exported &&
    exported.kMaxEventTargetListeners) {
  try {
    if (typeof EventTarget.prototype[exported.kMaxEventTargetListeners] !== 'number') {
      EventTarget.prototype[exported.kMaxEventTargetListeners] = exported.defaultMaxListeners;
    }
  } catch {}
}
if (typeof EventTarget === 'function' &&
    EventTarget.prototype &&
    exported &&
    exported.kMaxEventTargetListenersWarned) {
  try {
    if (typeof EventTarget.prototype[exported.kMaxEventTargetListenersWarned] !== 'boolean') {
      EventTarget.prototype[exported.kMaxEventTargetListenersWarned] = false;
    }
  } catch {}
}

if (typeof process === 'object' && process != null && !process.__unode_nexttick_patched) {
  process.__unode_nexttick_patched = true;
  const origNextTick = process.nextTick;
  const patchedNextTick = function patchedNextTick(fn, ...args) {
    if (typeof fn !== 'function') {
      const err = new TypeError(
        `The "callback" argument must be of type function. Received type ${typeof fn} (${String(fn)})`,
      );
      err.code = 'ERR_INVALID_ARG_TYPE';
      throw err;
    }
    if (origNextTick && origNextTick !== process.nextTick) {
      Promise.resolve().then(() => fn(...args));
      return undefined;
    }
    Promise.resolve().then(() => fn(...args));
    return undefined;
  };
  try {
    Object.defineProperty(process, 'nextTick', {
      value: patchedNextTick,
      configurable: true,
      writable: true,
    });
  } catch {
    process.nextTick = patchedNextTick;
  }
}

const originalGetMaxListeners = exported.getMaxListeners;
exported.getMaxListeners = function getMaxListenersPatched(emitterOrTarget) {
  if (emitterOrTarget &&
      typeof emitterOrTarget === 'object' &&
      typeof emitterOrTarget.aborted === 'boolean') {
    return 0;
  }
  return originalGetMaxListeners(emitterOrTarget);
};

module.exports = exported;
