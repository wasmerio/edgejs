'use strict';

const kNone = 1 << 0;
const kSkipLog = 1 << 1;
const kSkipTrace = 1 << 2;

function initializeDebugEnv(_debugEnv) {}

function debuglog(_set, cb) {
  let initialized = false;
  let debugFn = () => {};
  const logger = (...args) => {
    if (!initialized) {
      initialized = true;
      if (typeof cb === 'function') cb(debugFn);
    }
    return debugFn(...args);
  };
  Object.defineProperty(logger, 'enabled', {
    __proto__: null,
    configurable: true,
    enumerable: true,
    get() {
      return false;
    },
  });
  return logger;
}

function debugWithTimer(_set, cb) {
  const startTimer = () => {};
  const endTimer = () => {};
  const logTimer = () => {};
  if (typeof cb === 'function') cb(startTimer, endTimer, logTimer);
  return { startTimer, endTimer, logTimer };
}

module.exports = {
  kNone,
  kSkipLog,
  kSkipTrace,
  debuglog,
  debugWithTimer,
  initializeDebugEnv,
};
