'use strict';

function emitWarningSync(message, type, code) {
  if (typeof process.emitWarning === 'function') {
    process.emitWarning(message, type, code);
  }
}

module.exports = {
  emitWarningSync,
};
