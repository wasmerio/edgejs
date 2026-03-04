'use strict';

const EventEmitter = require('events');

const kDomainStack = globalThis.__ubi_domain_stack || (globalThis.__ubi_domain_stack = []);
let kActiveDomain = null;

if (!Object.getOwnPropertyDescriptor(process, 'domain') ||
    typeof Object.getOwnPropertyDescriptor(process, 'domain').get !== 'function') {
  Object.defineProperty(process, 'domain', {
    configurable: true,
    enumerable: true,
    get() {
      return kActiveDomain;
    },
    set(v) {
      kActiveDomain = v || null;
      return kActiveDomain;
    },
  });
}

class Domain extends EventEmitter {
  enter() {
    kDomainStack.push(this);
    kActiveDomain = this;
    process.domain = this;
  }

  exit() {
    const idx = kDomainStack.lastIndexOf(this);
    if (idx >= 0) {
      kDomainStack.splice(idx);
    }
    kActiveDomain = kDomainStack.length > 0 ? kDomainStack[kDomainStack.length - 1] : null;
    process.domain = kActiveDomain;
  }

  add(emitter) {
    if (!emitter || typeof emitter.emit !== 'function') return;
    const domain = this;
    const originalEmit = emitter.emit;
    if (typeof emitter.__ubi_domain_emit === 'function') return;
    emitter.__ubi_domain_emit = originalEmit;
    emitter.emit = function domainEmit(type, ...args) {
      if (type === 'error' && emitter.listenerCount('error') === 0) {
        let err = args[0];
        if (err === undefined || err === null || err === false) {
          err = new Error('Unhandled "error" event');
        }
        domain.emit('error', err);
        return false;
      }
      return originalEmit.call(this, type, ...args);
    };
  }

  run(fn, ...args) {
    if (typeof fn !== 'function') return;
    this.enter();
    const ret = fn.apply(this, args);
    this.exit();
    return ret;
  }

  bind(fn) {
    const domain = this;
    return function boundDomain(...args) {
      domain.enter();
      const ret = fn.apply(this, args);
      domain.exit();
      return ret;
    };
  }
}

if (!globalThis.__ubi_domain_timer_patch_installed) {
  globalThis.__ubi_domain_timer_patch_installed = true;
  const bindCallbackToDomain = (cb) => {
    if (typeof cb !== 'function') return cb;
    const d = process.domain;
    if (!d || typeof d.bind !== 'function') return cb;
    return d.bind(cb);
  };
  const nativeSetImmediate = globalThis.setImmediate;
  if (typeof nativeSetImmediate === 'function') {
    globalThis.setImmediate = function domainSetImmediate(cb, ...args) {
      return nativeSetImmediate.call(this, bindCallbackToDomain(cb), ...args);
    };
  }
  const nativeSetTimeout = globalThis.setTimeout;
  if (typeof nativeSetTimeout === 'function') {
    globalThis.setTimeout = function domainSetTimeout(cb, ms, ...args) {
      return nativeSetTimeout.call(this, bindCallbackToDomain(cb), ms, ...args);
    };
  }
  const nativeNextTick = process.nextTick;
  if (typeof nativeNextTick === 'function') {
    process.nextTick = function domainNextTick(cb, ...args) {
      return nativeNextTick.call(process, bindCallbackToDomain(cb), ...args);
    };
  }
}

function create() {
  return new Domain();
}

module.exports = {
  Domain,
  create,
  get active() {
    return process.domain;
  },
};
