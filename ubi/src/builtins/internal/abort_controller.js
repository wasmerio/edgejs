'use strict';

function ensureDomException() {
  if (typeof globalThis.DOMException === 'function') {
    return globalThis.DOMException;
  }
  class UbiDOMException extends Error {
    constructor(message = '', name = 'Error') {
      super(String(message));
      this.name = String(name || 'Error');
    }
  }
  globalThis.DOMException = UbiDOMException;
  return UbiDOMException;
}

const DOMExceptionImpl = ensureDomException();

class AbortSignal extends EventTarget {
  constructor() {
    super();
    this.aborted = false;
    this.reason = undefined;
  }

  throwIfAborted() {
    if (this.aborted) throw this.reason;
  }

  __abort(reason) {
    if (this.aborted) return;
    this.aborted = true;
    this.reason = reason;
    const event = new Event('abort');
    this.dispatchEvent(event);
    if (typeof this.onabort === 'function') {
      this.onabort(event);
    }
  }

  static abort(reason = new DOMExceptionImpl('This operation was aborted', 'AbortError')) {
    const signal = new AbortSignal();
    signal.__abort(reason);
    return signal;
  }

  static timeout(delay) {
    const ms = Number(delay);
    if (!Number.isFinite(ms) || ms < 0 || !Number.isInteger(ms)) {
      throw new RangeError('The value of "delay" is out of range.');
    }
    const signal = new AbortSignal();
    const timeout = setTimeout(() => {
      signal.__abort(new DOMExceptionImpl('The operation was aborted due to timeout', 'TimeoutError'));
    }, ms);
    if (timeout && typeof timeout.unref === 'function') timeout.unref();
    return signal;
  }

  static any(signals) {
    if (!Array.isArray(signals)) {
      throw new TypeError('The "signals" argument must be an instance of Array');
    }
    const composite = new AbortSignal();
    for (let i = 0; i < signals.length; i++) {
      const signal = signals[i];
      if (!(signal instanceof AbortSignal)) {
        continue;
      }
      if (signal.aborted) {
        composite.__abort(signal.reason);
        return composite;
      }
      signal.addEventListener('abort', () => composite.__abort(signal.reason), { once: true });
    }
    return composite;
  }
}

class AbortController {
  constructor() {
    this.signal = new AbortSignal();
  }

  abort(reason = new DOMExceptionImpl('This operation was aborted', 'AbortError')) {
    this.signal.__abort(reason);
  }
}

function ensureMessageChannel() {
  if (typeof globalThis.MessageChannel === 'function' && typeof globalThis.MessagePort === 'function') {
    return;
  }

  class MessagePort extends EventTarget {
    constructor() {
      super();
      this.onmessage = null;
      this.onmessageerror = null;
      this.__peer = null;
      this.__closed = false;
    }

    postMessage(data /* , transferList */) {
      if (this.__closed || this.__peer === null || this.__peer.__closed) return;
      const peer = this.__peer;
      queueMicrotask(() => {
        if (peer.__closed) return;
        const event = new Event('message');
        Object.defineProperty(event, 'data', {
          configurable: true,
          enumerable: true,
          writable: false,
          value: data,
        });
        if (typeof peer.onmessage === 'function') {
          peer.onmessage(event);
        }
        peer.dispatchEvent(event);
      });
    }

    start() {}

    close() {
      this.__closed = true;
      if (this.__peer !== null) {
        this.__peer.__peer = null;
        this.__peer = null;
      }
    }

    ref() {
      return this;
    }

    unref() {
      return this;
    }

    hasRef() {
      return false;
    }
  }

  class MessageChannel {
    constructor() {
      this.port1 = new MessagePort();
      this.port2 = new MessagePort();
      this.port1.__peer = this.port2;
      this.port2.__peer = this.port1;
    }
  }

  globalThis.MessagePort = MessagePort;
  globalThis.MessageChannel = MessageChannel;
}

function transferableAbortSignal(signal) {
  if (!(signal instanceof AbortSignal)) {
    throw new TypeError('The "signal" argument must be an AbortSignal');
  }
  return signal;
}

function transferableAbortController() {
  return new AbortController();
}

function aborted(signal) {
  if (!(signal instanceof AbortSignal)) {
    return Promise.reject(new TypeError('The "signal" argument must be an AbortSignal'));
  }
  if (signal.aborted) return Promise.resolve();
  return new Promise((resolve) => {
    signal.addEventListener('abort', resolve, { once: true });
  });
}

ensureMessageChannel();
globalThis.AbortSignal = AbortSignal;
globalThis.AbortController = AbortController;

module.exports = {
  AbortController,
  AbortSignal,
  ClonedAbortSignal: AbortSignal,
  aborted,
  transferableAbortSignal,
  transferableAbortController,
};
