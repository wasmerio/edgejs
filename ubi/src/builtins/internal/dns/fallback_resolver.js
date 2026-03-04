'use strict';

const dgram = require('dgram');
const { isIP } = require('internal/net');
const {
  DNSException,
} = require('internal/errors');

const {
  UV_ENOSYS,
} = internalBinding('uv');

const kPendingQueriesByHandle = new WeakMap();

const kTypeByBindingName = {
  queryA: 1,
  queryNs: 2,
  queryCname: 5,
  querySoa: 6,
  queryPtr: 12,
  queryMx: 15,
  queryTxt: 16,
  queryAaaa: 28,
  querySrv: 33,
  queryNaptr: 35,
  queryAny: 255,
  queryCaa: 257,
  queryTlsa: 52,
  getHostByAddr: 12,
};

const kRCodeToError = {
  1: 'EFORMERR',
  2: 'ESERVFAIL',
  3: 'ENOTFOUND',
  4: 'ENOTIMP',
  5: 'EREFUSED',
};

function shouldUseFallback(err) {
  return err === UV_ENOSYS;
}

function getPendingHandle(resolver) {
  if (resolver && typeof resolver === 'object' && resolver._handle) {
    return resolver._handle;
  }
  return resolver;
}

function getPendingSet(resolver) {
  const handle = getPendingHandle(resolver);
  if (!handle || (typeof handle !== 'object' && typeof handle !== 'function')) {
    return new Set();
  }
  let set = kPendingQueriesByHandle.get(handle);
  if (!set) {
    set = new Set();
    kPendingQueriesByHandle.set(handle, set);
  }
  return set;
}

function hasPendingQueries(resolver) {
  const handle = getPendingHandle(resolver);
  const set = handle ? kPendingQueriesByHandle.get(handle) : null;
  return !!(set && set.size > 0);
}

function cancelPendingQueries(resolver) {
  const handle = getPendingHandle(resolver);
  const set = handle ? kPendingQueriesByHandle.get(handle) : null;
  if (!set || set.size === 0) return;
  for (const pending of Array.from(set)) {
    pending.cancel();
  }
}

function encodeDomainName(name) {
  if (!name) return Buffer.from([0]);
  const labels = String(name).split('.');
  const chunks = [];
  for (const label of labels) {
    const raw = Buffer.from(label, 'ascii');
    chunks.push(Buffer.from([raw.length]));
    chunks.push(raw);
  }
  chunks.push(Buffer.from([0]));
  return Buffer.concat(chunks);
}

function buildQueryPacket(id, hostname, qtype) {
  const header = Buffer.allocUnsafe(12);
  header.writeUInt16BE(id & 0xffff, 0);
  header.writeUInt16BE(0x0100, 2); // RD=1
  header.writeUInt16BE(1, 4); // QDCOUNT
  header.writeUInt16BE(0, 6); // ANCOUNT
  header.writeUInt16BE(0, 8); // NSCOUNT
  header.writeUInt16BE(0, 10); // ARCOUNT
  const qname = encodeDomainName(hostname);
  const tail = Buffer.allocUnsafe(4);
  tail.writeUInt16BE(qtype, 0);
  tail.writeUInt16BE(1, 2); // IN
  return Buffer.concat([header, qname, tail]);
}

function readName(packet, offset, depth = 0) {
  if (depth > 16) throw new Error('DNS name pointer recursion');
  const labels = [];
  let cursor = offset;
  let consumed = 0;
  let jumped = false;
  while (cursor < packet.length) {
    const len = packet[cursor];
    if (len === 0) {
      if (!jumped) consumed += 1;
      return { name: labels.join('.'), nextOffset: offset + consumed };
    }
    if ((len & 0xc0) === 0xc0) {
      if (cursor + 1 >= packet.length) throw new Error('Truncated DNS name pointer');
      const ptr = ((len & 0x3f) << 8) | packet[cursor + 1];
      const nested = readName(packet, ptr, depth + 1);
      if (nested.name) labels.push(nested.name);
      if (!jumped) consumed += 2;
      jumped = true;
      return { name: labels.join('.'), nextOffset: offset + consumed };
    }
    const start = cursor + 1;
    const end = start + len;
    if (end > packet.length) throw new Error('Truncated DNS name label');
    labels.push(packet.toString('ascii', start, end));
    cursor = end;
    if (!jumped) consumed += len + 1;
  }
  throw new Error('Unterminated DNS name');
}

function formatIPv6(bytes) {
  const parts = [];
  for (let i = 0; i < 16; i += 2) {
    parts.push(((bytes[i] << 8) | bytes[i + 1]).toString(16));
  }
  let bestStart = -1;
  let bestLen = 0;
  let curStart = -1;
  let curLen = 0;
  for (let i = 0; i <= parts.length; i++) {
    const isZero = i < parts.length && parts[i] === '0';
    if (isZero) {
      if (curStart === -1) curStart = i;
      curLen += 1;
    } else if (curStart !== -1) {
      if (curLen > bestLen) {
        bestStart = curStart;
        bestLen = curLen;
      }
      curStart = -1;
      curLen = 0;
    }
  }
  if (bestLen > 1) {
    const left = parts.slice(0, bestStart).join(':');
    const right = parts.slice(bestStart + bestLen).join(':');
    if (left.length === 0 && right.length === 0) return '::';
    if (left.length === 0) return `::${right}`;
    if (right.length === 0) return `${left}::`;
    return `${left}::${right}`;
  }
  return parts.join(':');
}

function parseRecordData(packet, type, rdOffset, rdLength) {
  const rdEnd = rdOffset + rdLength;
  if (rdEnd > packet.length) throw new Error('Truncated DNS RR data');
  switch (type) {
    case 1: { // A
      if (rdLength !== 4) throw new Error('Invalid A record length');
      return {
        type: 'A',
        address: `${packet[rdOffset]}.${packet[rdOffset + 1]}.${packet[rdOffset + 2]}.${packet[rdOffset + 3]}`,
      };
    }
    case 28: { // AAAA
      if (rdLength !== 16) throw new Error('Invalid AAAA record length');
      return {
        type: 'AAAA',
        address: formatIPv6(packet.subarray(rdOffset, rdEnd)),
      };
    }
    case 2: // NS
    case 5: // CNAME
    case 12: { // PTR
      const { name } = readName(packet, rdOffset);
      const typeName = type === 2 ? 'NS' : (type === 5 ? 'CNAME' : 'PTR');
      return { type: typeName, value: name };
    }
    case 15: { // MX
      if (rdLength < 3) throw new Error('Invalid MX record length');
      const priority = packet.readUInt16BE(rdOffset);
      const { name } = readName(packet, rdOffset + 2);
      return { type: 'MX', priority, exchange: name };
    }
    case 16: { // TXT
      const entries = [];
      let p = rdOffset;
      while (p < rdEnd) {
        const len = packet[p];
        p += 1;
        if (p + len > rdEnd) throw new Error('Invalid TXT chunk length');
        entries.push(packet.toString('utf8', p, p + len));
        p += len;
      }
      return { type: 'TXT', entries };
    }
    case 6: { // SOA
      const mname = readName(packet, rdOffset);
      const rname = readName(packet, mname.nextOffset);
      const trailer = rname.nextOffset;
      if (trailer + 20 > rdEnd) throw new Error('Invalid SOA record length');
      return {
        type: 'SOA',
        nsname: mname.name,
        hostmaster: rname.name,
        serial: packet.readUInt32BE(trailer),
        refresh: packet.readUInt32BE(trailer + 4),
        retry: packet.readUInt32BE(trailer + 8),
        expire: packet.readUInt32BE(trailer + 12),
        minttl: packet.readUInt32BE(trailer + 16),
      };
    }
    case 257: { // CAA
      if (rdLength < 2) throw new Error('Invalid CAA record length');
      const critical = packet[rdOffset];
      const tagLen = packet[rdOffset + 1];
      const tagStart = rdOffset + 2;
      const tagEnd = tagStart + tagLen;
      if (tagEnd > rdEnd) throw new Error('Invalid CAA tag length');
      const value = packet.toString('utf8', tagEnd, rdEnd);
      const tag = packet.toString('ascii', tagStart, tagEnd);
      const out = { type: 'CAA', critical };
      if (tag === 'issue') out.issue = value;
      else if (tag === 'issuewild') out.issuewild = value;
      else if (tag === 'iodef') out.iodef = value;
      else out[tag] = value;
      return out;
    }
    default:
      return null;
  }
}

function parseResponse(packet, expectedId) {
  if (!Buffer.isBuffer(packet) || packet.length < 12) {
    throw new Error('DNS response too short');
  }
  const id = packet.readUInt16BE(0);
  if (id !== expectedId) throw new Error('DNS response id mismatch');
  const flags = packet.readUInt16BE(2);
  const rcode = flags & 0x000f;
  const qdCount = packet.readUInt16BE(4);
  const anCount = packet.readUInt16BE(6);
  const nsCount = packet.readUInt16BE(8);
  const arCount = packet.readUInt16BE(10);
  let offset = 12;
  for (let i = 0; i < qdCount; i++) {
    const q = readName(packet, offset);
    offset = q.nextOffset + 4;
    if (offset > packet.length) throw new Error('Truncated DNS question');
  }
  const answers = [];
  const totalRecords = anCount + nsCount + arCount;
  for (let i = 0; i < totalRecords; i++) {
    const rr = readName(packet, offset);
    offset = rr.nextOffset;
    if (offset + 10 > packet.length) throw new Error('Truncated DNS RR header');
    const type = packet.readUInt16BE(offset);
    const cls = packet.readUInt16BE(offset + 2);
    const ttl = packet.readUInt32BE(offset + 4);
    const rdLength = packet.readUInt16BE(offset + 8);
    const rdOffset = offset + 10;
    offset = rdOffset + rdLength;
    if (offset > packet.length) throw new Error('Truncated DNS RR payload');
    if (i < anCount && cls === 1) {
      const parsed = parseRecordData(packet, type, rdOffset, rdLength);
      if (parsed) {
        parsed.ttl = ttl;
        answers.push(parsed);
      }
    }
  }
  if (offset !== packet.length) throw new Error('Malformed DNS packet length');
  return { rcode, answers };
}

function normalizeServers(servers) {
  if (!Array.isArray(servers) || servers.length === 0) {
    return [['127.0.0.1', 53]];
  }
  const out = [];
  for (const server of servers) {
    if (!Array.isArray(server) || server.length < 2) continue;
    const host = String(server[0]);
    const port = Number(server[1]) || 53;
    out.push([host, port]);
  }
  return out.length > 0 ? out : [['127.0.0.1', 53]];
}

function reverseLookupName(hostname) {
  const family = isIP(hostname);
  if (family === 4) {
    return String(hostname).split('.').reverse().join('.') + '.in-addr.arpa';
  }
  if (family === 6) {
    const input = String(hostname).toLowerCase();
    const parts = input.split('::');
    const left = parts[0] ? parts[0].split(':').filter(Boolean) : [];
    const right = parts[1] ? parts[1].split(':').filter(Boolean) : [];
    const missing = Math.max(0, 8 - left.length - right.length);
    const full = [
      ...left,
      ...new Array(missing).fill('0'),
      ...right,
    ].map((chunk) => chunk.padStart(4, '0'));
    const hex = full.join('');
    return hex.split('').reverse().join('.') + '.ip6.arpa';
  }
  return hostname;
}

function toResult(bindingName, answers, ttl) {
  const pick = (type) => answers.filter((rr) => rr.type === type);
  switch (bindingName) {
    case 'queryAny':
      return answers.map((rr) => {
        if (rr.type === 'TXT') return { type: 'TXT', entries: rr.entries, ttl: rr.ttl };
        if (rr.type === 'SOA') {
          return {
            type: 'SOA',
            nsname: rr.nsname,
            hostmaster: rr.hostmaster,
            serial: rr.serial,
            refresh: rr.refresh,
            retry: rr.retry,
            expire: rr.expire,
            minttl: rr.minttl,
            ttl: rr.ttl,
          };
        }
        if (rr.type === 'MX') return { type: 'MX', exchange: rr.exchange, priority: rr.priority, ttl: rr.ttl };
        if (rr.type === 'NS' || rr.type === 'CNAME' || rr.type === 'PTR') return { type: rr.type, value: rr.value, ttl: rr.ttl };
        if (rr.type === 'CAA') {
          const out = { type: 'CAA', critical: rr.critical, ttl: rr.ttl };
          if (rr.issue !== undefined) out.issue = rr.issue;
          if (rr.issuewild !== undefined) out.issuewild = rr.issuewild;
          if (rr.iodef !== undefined) out.iodef = rr.iodef;
          return out;
        }
        return { type: rr.type, address: rr.address, ttl: rr.ttl };
      });
    case 'queryA': {
      const rows = pick('A');
      if (rows.length === 0) return [];
      return ttl ? rows.map((rr) => ({ address: rr.address, ttl: rr.ttl })) :
        rows.map((rr) => rr.address);
    }
    case 'queryAaaa': {
      const rows = pick('AAAA');
      if (rows.length === 0) return [];
      return ttl ? rows.map((rr) => ({ address: rr.address, ttl: rr.ttl })) :
        rows.map((rr) => rr.address);
    }
    case 'querySoa': {
      const row = pick('SOA')[0];
      if (!row) return null;
      return {
        nsname: row.nsname,
        hostmaster: row.hostmaster,
        serial: row.serial,
        refresh: row.refresh,
        retry: row.retry,
        expire: row.expire,
        minttl: row.minttl,
      };
    }
    case 'queryMx':
      return pick('MX').map((rr) => ({ exchange: rr.exchange, priority: rr.priority }));
    case 'queryNs':
      return pick('NS').map((rr) => rr.value);
    case 'queryCname':
      return pick('CNAME').map((rr) => rr.value);
    case 'queryPtr':
      return pick('PTR').map((rr) => rr.value);
    case 'getHostByAddr':
      return pick('PTR').map((rr) => rr.value);
    case 'queryTxt':
      return pick('TXT').map((rr) => rr.entries);
    case 'queryCaa':
      return pick('CAA').map((rr) => {
        const out = { critical: rr.critical };
        if (rr.issue !== undefined) out.issue = rr.issue;
        if (rr.issuewild !== undefined) out.issuewild = rr.issuewild;
        if (rr.iodef !== undefined) out.iodef = rr.iodef;
        return out;
      });
    default:
      return null;
  }
}

function computeTimeoutMs(resolver, attemptIndex) {
  const rawBase = Number(resolver?._queryTimeout);
  const base = Number.isFinite(rawBase) && rawBase >= 0 ? rawBase : 5000;
  let timeout = base === 0 ? 1 : (base * (2 ** attemptIndex));
  const maxTimeout = Number(resolver?._queryMaxTimeout);
  if (Number.isFinite(maxTimeout) && maxTimeout > 0) {
    timeout = Math.min(timeout, maxTimeout);
  }
  return Math.max(1, Math.trunc(timeout));
}

function getTries(resolver) {
  const tries = Number(resolver?._queryTries);
  if (!Number.isFinite(tries) || tries <= 0) return 4;
  return Math.trunc(tries);
}

function makeDnsError(code, bindingName, hostname) {
  return new DNSException(code, bindingName, hostname);
}

function queryOneServer(server, packet, id, timeoutMs, state) {
  return new Promise((resolve, reject) => {
    const [host, port] = server;
    const family = isIP(host) === 6 ? 'udp6' : 'udp4';
    const socket = dgram.createSocket(family);
    let done = false;

    const finish = (fn, value) => {
      if (done) return;
      done = true;
      clearTimeout(timer);
      socket.removeAllListeners();
      try {
        socket.close();
      } catch {}
      state.socket = null;
      fn(value);
    };

    state.socket = socket;
    state.cancel = () => {
      finish(reject, makeDnsError('ECANCELLED', state.bindingName, state.hostname));
    };

    socket.once('error', () => {
      finish(reject, makeDnsError('ECONNREFUSED', state.bindingName, state.hostname));
    });

    socket.once('message', (msg) => {
      try {
        const parsed = parseResponse(msg, id);
        if (parsed.rcode !== 0) {
          const code = kRCodeToError[parsed.rcode] || 'EBADRESP';
          finish(reject, makeDnsError(code, state.bindingName, state.hostname));
          return;
        }
        finish(resolve, parsed.answers);
      } catch {
        finish(reject, makeDnsError('EBADRESP', state.bindingName, state.hostname));
      }
    });

    const timer = setTimeout(() => {
      finish(reject, makeDnsError('ETIMEOUT', state.bindingName, state.hostname));
    }, timeoutMs);

    socket.send(packet, port, host, (err) => {
      if (err) {
        finish(reject, makeDnsError('ECONNREFUSED', state.bindingName, state.hostname));
      }
    });
  });
}

async function resolveWithFallback(resolver, bindingName, hostname, ttl) {
  const qtype = kTypeByBindingName[bindingName];
  if (!qtype) {
    throw makeDnsError('ENOTIMP', bindingName, hostname);
  }
  const id = (Math.random() * 0xffff) & 0xffff;
  const queryHost = bindingName === 'getHostByAddr' ? reverseLookupName(hostname) : hostname;
  const packet = buildQueryPacket(id, queryHost, qtype);
  const servers = normalizeServers(resolver?._handle?.getServers?.());
  const tries = getTries(resolver);
  const pendingSet = getPendingSet(resolver);
  const state = {
    bindingName,
    hostname,
    socket: null,
    cancel: null,
  };

  const pending = {
    cancel() {
      if (typeof state.cancel === 'function') {
        state.cancel();
      }
    },
  };
  pendingSet.add(pending);

  try {
    let lastErr = makeDnsError('ETIMEOUT', bindingName, hostname);
    for (let attempt = 0; attempt < tries; attempt++) {
      const timeoutMs = computeTimeoutMs(resolver, attempt);
      const server = servers[attempt % servers.length];
      try {
        const answers = await queryOneServer(server, packet, id, timeoutMs, state);
        const result = toResult(bindingName, answers, ttl);
        if (result === null ||
            (Array.isArray(result) && result.length === 0) ||
            (bindingName === 'querySoa' && !result)) {
          throw makeDnsError('ENOTFOUND', bindingName, hostname);
        }
        return result;
      } catch (err) {
        lastErr = err;
        if (err?.code === 'ECANCELLED') throw err;
        if (attempt === tries - 1) throw err;
        if (err?.code !== 'ETIMEOUT' && err?.code !== 'EBADRESP' && err?.code !== 'ECONNREFUSED') {
          throw err;
        }
      }
    }
    throw lastErr;
  } finally {
    pendingSet.delete(pending);
    if (pendingSet.size === 0) {
      const handle = getPendingHandle(resolver);
      if (handle) {
        kPendingQueriesByHandle.delete(handle);
      }
    }
  }
}

function toResolverLike(handle, timeout, tries, maxTimeout) {
  return {
    _handle: handle,
    _queryTimeout: timeout,
    _queryTries: tries,
    _queryMaxTimeout: maxTimeout,
  };
}

function extractErrorCode(error) {
  if (!error) return 'EBADRESP';
  if (typeof error === 'string' || typeof error === 'number') return error;
  return error.code ?? error.errno ?? 'EBADRESP';
}

function invokeReqOncomplete(req, args) {
  const fn = req?.oncomplete;
  if (typeof fn !== 'function') return;
  try {
    fn.apply(req, args);
  } catch {
    // Swallow errors to match native callback boundaries.
  }
}

function startNativeQuery(handle, req, bindingName, hostname, ttl, timeout, tries, maxTimeout) {
  const resolver = toResolverLike(handle, timeout, tries, maxTimeout);
  resolveWithFallback(resolver, bindingName, hostname, !!ttl)
    .then((result) => {
      invokeReqOncomplete(req, [0, result]);
    })
    .catch((error) => {
      invokeReqOncomplete(req, [extractErrorCode(error)]);
    });
  return 0;
}

function cancelNativeQueries(handle) {
  cancelPendingQueries({ _handle: handle });
}

function hasNativeQueries(handle) {
  return hasPendingQueries({ _handle: handle });
}

module.exports = {
  shouldUseFallback,
  resolveWithFallback,
  hasPendingQueries,
  cancelPendingQueries,
  startNativeQuery,
  cancelNativeQueries,
  hasNativeQueries,
};
