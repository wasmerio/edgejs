'use strict';
if (process && typeof process.off !== 'function' && typeof process.removeListener === 'function') {
  process.off = process.removeListener.bind(process);
}
if (process && typeof process.kill !== 'function') {
  process.kill = function kill(pid, signal) {
    if (Number(pid) === Number(process.pid) && String(signal || 'SIGTERM').toUpperCase() === 'SIGINT') {
      if (typeof process.emit === 'function') process.emit('SIGINT');
      return true;
    }
    return true;
  };
}
module.exports = require('../../../node-lib/util.js');
