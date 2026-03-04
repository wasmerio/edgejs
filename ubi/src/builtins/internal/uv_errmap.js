'use strict';

const kUvErrMap = new Map([
  [-2, ['ENOENT', 'no such file or directory']],
  [-9, ['EBADF', 'bad file descriptor']],
  [-12, ['ENOMEM', 'out of memory']],
  [-13, ['EACCES', 'permission denied']],
  [-48, ['EADDRINUSE', 'address already in use']],
  [-22, ['EINVAL', 'invalid argument']],
  [-38, ['ENOSYS', 'function not implemented']],
  [-54, ['ECONNRESET', 'connection reset by peer']],
  [-61, ['ECONNREFUSED', 'connection refused']],
  [-49, ['EADDRNOTAVAIL', 'address not available']],
  [-55, ['ENOBUFS', 'no buffer space available']],
  [-60, ['ETIMEDOUT', 'connection timed out']],
  [-98, ['EADDRINUSE', 'address already in use']],
  [-104, ['ECONNRESET', 'connection reset by peer']],
  [-105, ['ENOBUFS', 'no buffer space available']],
  [-110, ['ETIMEDOUT', 'connection timed out']],
  [-111, ['ECONNREFUSED', 'connection refused']],
  [-3007, ['ENOTFOUND', 'name does not resolve']],
  [-3008, ['ENOTFOUND', 'name does not resolve']],
]);

function getUvErrorEntry(err) {
  const n = Number(err);
  if (!Number.isFinite(n)) return undefined;
  if (kUvErrMap.has(n)) return kUvErrMap.get(n);

  let name;
  try {
    const errno = require('os').constants && require('os').constants.errno;
    if (errno && typeof errno === 'object') {
      for (const key of Object.keys(errno)) {
        if (-Number(errno[key]) === n) {
          name = key;
          break;
        }
      }
    }
  } catch {}
  if (!name) return undefined;

  let message = name;
  try {
    if (typeof process?.binding === 'function') {
      const uv = process.binding('uv');
      if (uv && typeof uv.getErrorMessage === 'function') {
        const maybeMessage = uv.getErrorMessage(n);
        if (typeof maybeMessage === 'string' &&
            maybeMessage.length > 0 &&
            !maybeMessage.startsWith('Unknown system error')) {
          message = maybeMessage;
        }
      }
    }
  } catch {}

  const entry = [name, message];
  kUvErrMap.set(n, entry);
  return entry;
}

function getUvErrorMap() {
  return kUvErrMap;
}

function getUvErrorMessage(err) {
  const row = getUvErrorEntry(err);
  return row ? String(row[1]) : `Unknown system error ${String(err)}`;
}

module.exports = {
  getUvErrorEntry,
  getUvErrorMap,
  getUvErrorMessage,
};
