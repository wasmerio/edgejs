'use strict';

const kNoPromiseHook = Symbol('kNoPromiseHook');
const async_id_symbol = Symbol('async_id_symbol');
const owner_symbol = Symbol('owner_symbol');
const trigger_async_id_symbol = Symbol('trigger_async_id_symbol');

function noop() {}
function noopZero() { return 0; }

class AsyncResource {
  constructor(type) {
    this.type = type || 'AsyncResource';
  }

  runInAsyncScope(fn, thisArg, ...args) {
    return Reflect.apply(fn, thisArg, args);
  }

  emitDestroy() {}
}

module.exports = {
  AsyncResource,
  kNoPromiseHook,
  newAsyncId() { return 1; },
  getDefaultTriggerAsyncId() { return 0; },
  getOrSetAsyncId(obj) {
    if (obj == null || typeof obj !== 'object') return 0;
    if (typeof obj[async_id_symbol] !== 'number') obj[async_id_symbol] = 1;
    return obj[async_id_symbol];
  },
  defaultTriggerAsyncIdScope(_id, fn, ...args) {
    if (typeof fn === 'function') return Reflect.apply(fn, null, args);
  },
  symbols: {
    async_id_symbol,
    owner_symbol,
    trigger_async_id_symbol,
  },
  executionAsyncId() { return 0; },
  triggerAsyncId() { return 0; },
  executionAsyncResource() { return {}; },
  emitInit: noopZero,
  emitBefore: noopZero,
  emitDestroy: noopZero,
  clearDefaultTriggerAsyncId: noop,
  clearAsyncIdStack: noop,
  initHooksExist() { return false; },
  hasAsyncIdStack() { return false; },
  afterHooksExist() { return false; },
  emitAfter: noop,
  pushAsyncContext: noop,
  popAsyncContext: noop,
};
