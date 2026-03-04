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

let kDidPopulateErrnoMap = false;
let kErrnoConstants;

function getErrnoConstants() {
  if (kErrnoConstants !== undefined) return kErrnoConstants;
  try {
    const os = require('os');
    kErrnoConstants = os?.constants?.errno && typeof os.constants.errno === 'object' ?
      os.constants.errno :
      null;
  } catch {
    kErrnoConstants = null;
  }
  return kErrnoConstants;
}

function getUvMessageFromBinding(err) {
  try {
    if (typeof process?.binding === 'function') {
      const uv = process.binding('uv');
      if (uv && typeof uv.getErrorMessage === 'function') {
        const message = uv.getErrorMessage(err);
        if (typeof message === 'string' &&
            message.length > 0 &&
            !message.startsWith('Unknown system error')) {
          return message;
        }
      }
    }
  } catch {}
  return undefined;
}

function maybePopulateUvErrMapFromErrnoConstants() {
  if (kDidPopulateErrnoMap) return;
  kDidPopulateErrnoMap = true;

  const errno = getErrnoConstants();
  if (!errno) return;

  for (const key of Object.keys(errno)) {
    // Skip getaddrinfo symbolic names; these do not map 1:1 to libuv errno
    // negatives and can collide with standard errno values.
    if (!key.startsWith('E') || key.startsWith('EAI_')) continue;
    const raw = Number(errno[key]);
    if (!Number.isFinite(raw) || raw === 0) continue;
    const uvCode = raw > 0 ? -raw : raw;
    if (kUvErrMap.has(uvCode)) continue;
    const message = getUvMessageFromBinding(uvCode) ?? key;
    kUvErrMap.set(uvCode, [key, message]);
  }
}

function getUvErrorEntry(err) {
  maybePopulateUvErrMapFromErrnoConstants();

  const n = Number(err);
  if (!Number.isFinite(n)) return undefined;
  if (kUvErrMap.has(n)) return kUvErrMap.get(n);

  let name;
  const errno = getErrnoConstants();
  if (errno) {
    for (const key of Object.keys(errno)) {
      if (!key.startsWith('E') || key.startsWith('EAI_')) continue;
      const raw = Number(errno[key]);
      if (!Number.isFinite(raw) || raw === 0) continue;
      const uvCode = raw > 0 ? -raw : raw;
      if (uvCode === n) {
        name = key;
        break;
      }
    }
  }
  if (!name) return undefined;

  const message = getUvMessageFromBinding(n) ?? name;

  const entry = [name, message];
  kUvErrMap.set(n, entry);
  return entry;
}

function getUvErrorMap() {
  maybePopulateUvErrMapFromErrnoConstants();
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
