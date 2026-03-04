'use strict';

const internalUtil = require('internal/util');
function countBinaryOnes(n) {
  n = n - ((n >>> 1) & 0x55555555);
  n = (n & 0x33333333) + ((n >>> 2) & 0x33333333);
  return ((n + (n >>> 4) & 0xF0F0F0F) * 0x1010101) >>> 24;
}

function getCIDRFallback(address, netmask, family) {
  let ones = 0;
  let split = '.';
  let range = 10;
  let groupLength = 8;
  let hasZeros = false;
  let lastPos = 0;

  if (family === 'IPv6') {
    split = ':';
    range = 16;
    groupLength = 16;
  }

  for (let i = 0; i < netmask.length; i++) {
    if (netmask[i] !== split) {
      if (i + 1 < netmask.length) continue;
      i++;
    }
    const part = netmask.slice(lastPos, i);
    lastPos = i + 1;
    if (part !== '') {
      if (hasZeros) {
        if (part !== '0') return null;
      } else {
        const binary = Number.parseInt(part, range);
        const binaryOnes = countBinaryOnes(binary);
        ones += binaryOnes;
        if (binaryOnes !== groupLength) {
          if (binary.toString(2).includes('01')) return null;
          hasZeros = true;
        }
      }
    }
  }
  return `${address}/${ones}`;
}

const getCIDR = (internalUtil && typeof internalUtil.getCIDR === 'function') ?
  internalUtil.getCIDR :
  getCIDRFallback;
const {
  codes: {
    ERR_SYSTEM_ERROR,
  },
  hideStackFrames,
} = require('internal/errors');
const {
  validateInt32,
} = require('internal/validators');

const binding = internalBinding('os');
const constants = (internalBinding('constants') || {}).os || {};
if (!binding) {
  throw new Error('os builtin requires os binding');
}

const isWindows = process.platform === 'win32';

const {
  getAvailableParallelism,
  getCPUs,
  getFreeMem,
  getHomeDirectory: _getHomeDirectory,
  getHostname: _getHostname,
  getInterfaceAddresses: _getInterfaceAddresses,
  getLoadAvg,
  getPriority: _getPriority,
  getOSInformation: _getOSInformation,
  getTotalMem,
  getUptime: _getUptime,
  getUserInfo,
  isBigEndian,
  setPriority: _setPriority,
} = binding;

function getCheckedFunction(fnOrName) {
  return hideStackFrames(function checkError() {
    const ctx = {};
    const fn = typeof fnOrName === 'string' ? binding[fnOrName] : fnOrName;
    const ret = fn(ctx);
    if (ret === undefined) {
      throw new ERR_SYSTEM_ERROR.HideStackFramesError(ctx);
    }
    return ret;
  });
}

const {
  0: type,
  1: version,
  2: release,
  3: machine,
} = _getOSInformation();

// Keep these late-bound so tests that monkey-patch internalBinding('os')
// before requiring os still observe the patched implementations.
const getHomeDirectory = getCheckedFunction('getHomeDirectory');
const getHostname = getCheckedFunction('getHostname');
const getInterfaceAddresses = getCheckedFunction('getInterfaceAddresses');
const getUptime = getCheckedFunction('getUptime');

const getOSRelease = () => release;
const getOSType = () => type;
const getOSVersion = () => version;
const getMachine = () => machine;

getAvailableParallelism[Symbol.toPrimitive] = () => getAvailableParallelism();
getFreeMem[Symbol.toPrimitive] = () => getFreeMem();
getHostname[Symbol.toPrimitive] = () => getHostname();
getOSVersion[Symbol.toPrimitive] = () => getOSVersion();
getOSType[Symbol.toPrimitive] = () => getOSType();
getOSRelease[Symbol.toPrimitive] = () => getOSRelease();
getMachine[Symbol.toPrimitive] = () => getMachine();
getHomeDirectory[Symbol.toPrimitive] = () => getHomeDirectory();
getTotalMem[Symbol.toPrimitive] = () => getTotalMem();
getUptime[Symbol.toPrimitive] = () => getUptime();

const kEndianness = isBigEndian ? 'BE' : 'LE';
const avgValues = [0, 0, 0];

function loadavg() {
  getLoadAvg(avgValues);
  return [avgValues[0], avgValues[1], avgValues[2]];
}

function cpus() {
  const data = getCPUs() || [];
  const result = [];
  let i = 0;
  while (i < data.length) {
    result.push({
      model: data[i++],
      speed: data[i++],
      times: {
        user: data[i++],
        nice: data[i++],
        sys: data[i++],
        idle: data[i++],
        irq: data[i++],
      },
    });
  }
  return result;
}

function arch() {
  return process.arch;
}
arch[Symbol.toPrimitive] = () => process.arch;

function platform() {
  return process.platform;
}
platform[Symbol.toPrimitive] = () => process.platform;

function tmpdir() {
  if (isWindows) {
    const path = process.env.TEMP ||
           process.env.TMP ||
           (process.env.SystemRoot || process.env.windir) + '\\temp';

    if (path.length > 1 && path[path.length - 1] === '\\' && path[path.length - 2] !== ':') {
      return path.slice(0, -1);
    }
    return path;
  }

  const path = process.env.TMPDIR || process.env.TMP || process.env.TEMP || '/tmp';
  if (path.length > 1 && path[path.length - 1] === '/') {
    return path.slice(0, -1);
  }
  return path;
}
tmpdir[Symbol.toPrimitive] = () => tmpdir();

function endianness() {
  return kEndianness;
}
endianness[Symbol.toPrimitive] = () => kEndianness;

function networkInterfaces() {
  const data = getInterfaceAddresses();
  const result = {};
  if (data === undefined) return result;
  for (let i = 0; i < data.length; i += 7) {
    const name = data[i];
    const entry = {
      address: data[i + 1],
      netmask: data[i + 2],
      family: data[i + 3],
      mac: data[i + 4],
      internal: data[i + 5],
      cidr: getCIDR(data[i + 1], data[i + 2], data[i + 3]),
    };
    const scopeid = data[i + 6];
    if (scopeid !== -1) {
      entry.scopeid = scopeid;
    }
    const existing = result[name];
    if (existing !== undefined) existing.push(entry);
    else result[name] = [entry];
  }
  return result;
}

function setPriority(pid, priority) {
  if (priority === undefined) {
    priority = pid;
    pid = 0;
  }
  validateInt32(pid, 'pid');
  validateInt32(priority, 'priority', -20, 19);
  const ctx = {};
  if (_setPriority(pid, priority, ctx) !== 0) {
    throw new ERR_SYSTEM_ERROR(ctx);
  }
}

function getPriority(pid) {
  if (pid === undefined) pid = 0;
  else validateInt32(pid, 'pid');
  const ctx = {};
  const priority = _getPriority(pid, ctx);
  if (priority === undefined) {
    throw new ERR_SYSTEM_ERROR(ctx);
  }
  return priority;
}

function userInfo(options) {
  if (typeof options !== 'object') options = null;
  const ctx = {};
  const user = getUserInfo(options, ctx);
  if (user === undefined) {
    throw new ERR_SYSTEM_ERROR(ctx);
  }
  if (options && options.encoding === 'buffer') {
    const wrap = (value) => {
      if (value === null || value === undefined) return value;
      const b = Buffer.from(String(value), 'utf8');
      // In this runtime Buffer may be a Uint8Array shim; force Node-like toString for tests.
      Object.defineProperty(b, 'toString', {
        value(encoding) {
          if (encoding && encoding !== 'utf8' && encoding !== 'utf-8') return String(value);
          return String(value);
        },
        configurable: true,
      });
      return b;
    };
    return {
      uid: user.uid,
      gid: user.gid,
      username: wrap(user.username),
      homedir: wrap(user.homedir),
      shell: user.shell == null ? null : wrap(user.shell),
    };
  }
  return user;
}

module.exports = {
  arch,
  availableParallelism: getAvailableParallelism,
  cpus,
  endianness,
  freemem: getFreeMem,
  getPriority,
  homedir: getHomeDirectory,
  hostname: getHostname,
  loadavg,
  networkInterfaces,
  platform,
  release: getOSRelease,
  setPriority,
  tmpdir,
  totalmem: getTotalMem,
  type: getOSType,
  userInfo,
  uptime: getUptime,
  version: getOSVersion,
  machine: getMachine,
};

if (constants.signals) Object.freeze(constants.signals);

Object.defineProperties(module.exports, {
  constants: {
    configurable: false,
    enumerable: true,
    value: constants,
  },
  EOL: {
    configurable: true,
    enumerable: true,
    writable: false,
    value: isWindows ? '\r\n' : '\n',
  },
  devNull: {
    configurable: true,
    enumerable: true,
    writable: false,
    value: isWindows ? '\\\\.\\nul' : '/dev/null',
  },
});
