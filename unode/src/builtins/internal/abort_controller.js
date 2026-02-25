'use strict';

const AbortControllerImpl = globalThis.AbortController;
const AbortSignalImpl = globalThis.AbortSignal;

function transferableAbortSignal(signal) {
  return signal;
}

function transferableAbortController() {
  return new AbortControllerImpl();
}

function aborted(signal) {
  if (!signal || typeof signal !== 'object' || typeof signal.aborted !== 'boolean') {
    return Promise.reject(new TypeError('The "signal" argument must be an AbortSignal'));
  }
  if (signal.aborted) return Promise.resolve();
  return new Promise((resolve) => {
    signal.addEventListener('abort', resolve, { once: true });
  });
}

module.exports = {
  AbortController: AbortControllerImpl,
  AbortSignal: AbortSignalImpl,
  ClonedAbortSignal: AbortSignalImpl,
  aborted,
  transferableAbortSignal,
  transferableAbortController,
};
