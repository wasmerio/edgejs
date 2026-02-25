'use strict';

class MessagePort {
  postMessage(_value, transferList) {
    if (Array.isArray(transferList)) {
      for (const item of transferList) {
        if (!item || typeof item !== 'object') continue;
        const symbols = Object.getOwnPropertySymbols(item);
        for (const sym of symbols) {
          const key = String(sym);
          if (/untransferable/i.test(key) && item[sym]) {
            const err = new Error('Object could not be cloned.');
            err.name = 'DataCloneError';
            err.code = 25;
            throw err;
          }
        }
      }
    }
  }
  close() {}
}

class MessageChannel {
  constructor() {
    this.port1 = new MessagePort();
    this.port2 = new MessagePort();
  }
}

module.exports = {
  MessageChannel,
  isMainThread: true,
};
