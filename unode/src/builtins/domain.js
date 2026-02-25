'use strict';

const EventEmitter = require('events');

class Domain extends EventEmitter {
  add(emitter) {
    if (!emitter || typeof emitter.emit !== 'function') return;
    const domain = this;
    const originalEmit = emitter.emit;
    if (typeof emitter.__unode_domain_emit === 'function') return;
    emitter.__unode_domain_emit = originalEmit;
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
}

function create() {
  return new Domain();
}

module.exports = {
  Domain,
  create,
};
