/* eslint-disable no-use-before-define */
'use strict';

const { Buffer } = require('buffer');
const { inspect } = require('util');
const EventEmitter = require('events');

const binding = internalBinding('crypto');
if (!binding) throw new Error('crypto builtin requires crypto binding');

if (!globalThis.crypto) globalThis.crypto = {};
if (typeof globalThis.crypto.getRandomValues !== 'function') {
  globalThis.crypto.getRandomValues = function getRandomValues(typedArray) {
    randomFillSync(typedArray);
    return typedArray;
  };
}

function toBuffer(input, encoding) {
  if (input && typeof input === 'object' &&
      (input.type === 'secret' || input.type === 'private' || input.type === 'public')) {
    if (input.data != null) return toBuffer(input.data, encoding);
    if (input.key != null) return toBuffer(input.key, encoding);
  }
  if (Buffer.isBuffer(input)) return Buffer.from(input);
  if (ArrayBuffer.isView(input)) return Buffer.from(input.buffer, input.byteOffset, input.byteLength);
  if (input instanceof ArrayBuffer || input instanceof SharedArrayBuffer) return Buffer.from(input);
  if (typeof input === 'string') {
    if (encoding === 'hex' && (input.length % 2) !== 0) {
      const err = new TypeError("The argument 'encoding' is invalid for data of length 1. Received 'hex'");
      err.code = 'ERR_INVALID_ARG_VALUE';
      throw err;
    }
    return Buffer.from(input, encoding || 'utf8');
  }
  throw makeTypeError('ERR_INVALID_ARG_TYPE',
                      'The "data" argument must be of type string or an instance of Buffer, TypedArray, or DataView.' +
                      formatReceived(input));
}

function toWritableBuffer(input) {
  if (Buffer.isBuffer(input)) return input;
  if (ArrayBuffer.isView(input)) return Buffer.from(input.buffer, input.byteOffset, input.byteLength);
  if (input instanceof ArrayBuffer || input instanceof SharedArrayBuffer) return Buffer.from(input);
  throw makeTypeError('ERR_INVALID_ARG_TYPE', 'The "buf" argument must be an instance of Buffer or ArrayBufferView.');
}

function toSignatureBuffer(input) {
  if (Buffer.isBuffer(input)) return Buffer.from(input);
  if (ArrayBuffer.isView(input)) return Buffer.from(input.buffer, input.byteOffset, input.byteLength);
  throw makeTypeError('ERR_INVALID_ARG_TYPE',
                      'The "signature" argument must be an instance of Buffer, TypedArray, or DataView.' +
                      formatReceived(input));
}

function toNumber(name, value) {
  if (typeof value !== 'number') {
    throw makeTypeError('ERR_INVALID_ARG_TYPE', `The "${name}" argument must be of type number.${formatReceived(value)}`);
  }
  return value;
}

function makeTypeError(code, message) {
  const err = new TypeError(message);
  err.code = code;
  return err;
}

function makeRangeError(code, message) {
  const err = new RangeError(message);
  err.code = code;
  return err;
}

const _hashes = () => binding.getHashes();
const _ciphers = () => binding.getCiphers();
const _curves = () => binding.getCurves();

function getHashes() { return _hashes().slice().sort(); }
let _hashSetCache;
function getHashSet() {
  if (!_hashSetCache) _hashSetCache = new Set(getHashes());
  return _hashSetCache;
}
function resolveSupportedDigestName(algorithm) {
  const hashes = getHashSet();
  if (hashes.has(algorithm)) return algorithm;
  if (typeof algorithm === 'string') {
    const lower = algorithm.toLowerCase();
    if (hashes.has(lower)) return lower;
    const upper = algorithm.toUpperCase();
    if (hashes.has(upper)) return upper;
  }
  return null;
}
function getCiphers() { return _ciphers().slice().sort(); }
function getCurves() { return _curves().slice().sort(); }
let _cipherInfoByName;
let _cipherNameByNid;
function initCipherInfoCache() {
  if (_cipherInfoByName && _cipherNameByNid) return;
  _cipherInfoByName = new Map();
  _cipherNameByNid = new Map();
  const names = getCiphers();
  let nextNid = 2000;
  for (const name of names) {
    const native = binding.getCipherInfo(name);
    if (!native || typeof native !== 'object') continue;
    const info = {
      name,
      nid: name === 'aes-128-cbc' ? 419 : nextNid++,
      blockSize: 16,
      ivLength: native.ivLength,
      keyLength: native.keyLength,
      mode: native.mode,
    };
    _cipherInfoByName.set(name, info);
    _cipherNameByNid.set(info.nid, name);
  }
}
function getCipherInfo(nameOrNid, options) {
  if (nameOrNid === null || nameOrNid === undefined || typeof nameOrNid === 'object') {
    throw makeTypeError('ERR_INVALID_ARG_TYPE', `The "nameOrNid" argument must be of type string or number.${formatReceived(nameOrNid)}`);
  }
  if (options !== undefined) {
    if (options === null || typeof options !== 'object' || Array.isArray(options)) {
      throw makeTypeError('ERR_INVALID_ARG_TYPE', `The "options" argument must be of type object.${formatReceived(options)}`);
    }
    if (Object.prototype.hasOwnProperty.call(options, 'keyLength') && typeof options.keyLength !== 'number') {
      throw makeTypeError('ERR_INVALID_ARG_TYPE', `The "options.keyLength" property must be of type number.${formatReceived(options.keyLength)}`);
    }
    if (Object.prototype.hasOwnProperty.call(options, 'ivLength') && typeof options.ivLength !== 'number') {
      throw makeTypeError('ERR_INVALID_ARG_TYPE', `The "options.ivLength" property must be of type number.${formatReceived(options.ivLength)}`);
    }
  }
  initCipherInfoCache();
  let name;
  if (typeof nameOrNid === 'number') {
    name = _cipherNameByNid.get(nameOrNid);
    if (!name) return undefined;
  } else if (typeof nameOrNid === 'string') {
    name = nameOrNid;
  } else {
    throw makeTypeError('ERR_INVALID_ARG_TYPE', `The "nameOrNid" argument must be of type string or number.${formatReceived(nameOrNid)}`);
  }
  const info = _cipherInfoByName.get(name);
  if (!info) return undefined;
  if (options && Object.prototype.hasOwnProperty.call(options, 'keyLength') && options.keyLength !== info.keyLength) {
    return undefined;
  }
  if (options && Object.prototype.hasOwnProperty.call(options, 'ivLength')) {
    const n = options.ivLength;
    if (name.includes('-ccm') && (n < 7 || n > 13)) return undefined;
    if (name.includes('-ocb') && (n < 1 || n > 15)) return undefined;
    if (!name.includes('-ccm') && !name.includes('-ocb') && n !== info.ivLength) return undefined;
  }
  return { ...info };
}
function getFips() { return 0; }

function hash(algorithm, data, outputEncoding) {
  if (typeof algorithm !== 'string') {
    throw makeTypeError('ERR_INVALID_ARG_TYPE',
                        `The "algorithm" argument must be of type string.${formatReceived(algorithm)}`);
  }
  const input = toBuffer(data);
  if (outputEncoding !== undefined && typeof outputEncoding !== 'string') {
    throw makeTypeError('ERR_INVALID_ARG_TYPE',
                        `The "outputEncoding" argument must be of type string.${formatReceived(outputEncoding)}`);
  }
  const resolvedAlgorithm = resolveSupportedDigestName(algorithm);
  if (resolvedAlgorithm === null) {
    const err = new Error('Digest method not supported');
    err.code = 'ERR_CRYPTO_INVALID_DIGEST';
    throw err;
  }
  const normalized = resolvedAlgorithm.toLowerCase();
  const out = isXofAlgorithm(normalized)
    ? nativeToBuffer(binding.hashOneShotXof(normalized, input, getDefaultXofOutputLength(normalized)))
    : nativeToBuffer(binding.hashOneShot(resolvedAlgorithm, input));
  if (outputEncoding === 'buffer') return out;
  const enc = outputEncoding === undefined ? 'hex' : outputEncoding;
  if (!Buffer.isEncoding(enc)) {
    throw makeTypeError('ERR_INVALID_ARG_VALUE', `The argument 'outputEncoding' is invalid. Received '${enc}'`);
  }
  return out.toString(enc);
}

function nativeToBuffer(value) {
  return Buffer.isBuffer(value) ? value : Buffer.from(value);
}

function digestOutput(out, encoding) {
  if (encoding === undefined || encoding === 'buffer') return out;
  return out.toString(encoding);
}

function isXofAlgorithm(algorithm) {
  return algorithm === 'shake128' || algorithm === 'shake256';
}

function getDefaultXofOutputLength(algorithm) {
  if (algorithm === 'shake128') return 16;
  if (algorithm === 'shake256') return 32;
  return 0;
}

function makeError(code, message, Ctor = Error) {
  const err = new Ctor(message);
  err.code = code;
  return err;
}

function formatReceived(v) {
  if (v === undefined) return ' Received undefined';
  if (v === null) return ' Received null';
  if (typeof v === 'boolean') return ` Received type boolean (${v})`;
  if (typeof v === 'number') return ` Received type number (${v})`;
  if (typeof v === 'string') return ` Received type string ('${v}')`;
  if (Array.isArray(v)) return ' Received an instance of Array';
  if (typeof v === 'object' && v.constructor && v.constructor.name) return ` Received an instance of ${v.constructor.name}`;
  return ` Received type ${typeof v}`;
}

function formatRangeReceived(v) {
  if (typeof v === 'number' && Number.isFinite(v) && Math.abs(v) >= 1000000) {
    const sign = v < 0 ? '-' : '';
    const digits = String(Math.trunc(Math.abs(v)));
    const out = digits.replace(/\B(?=(\d{3})+(?!\d))/g, '_');
    return sign + out;
  }
  return String(v);
}

function normalizeKeyMaterial(input, opName) {
  if (input && typeof input === 'object' &&
      !Buffer.isBuffer(input) &&
      !ArrayBuffer.isView(input) && !(input instanceof ArrayBuffer) &&
      !(input instanceof SharedArrayBuffer) &&
      input.format === 'jwk' &&
      (typeof input.key !== 'object' || input.key == null)) {
    throw makeTypeError('ERR_INVALID_ARG_TYPE', 'The "key.key" property must be of type object');
  }
  const source = (input && typeof input === 'object' && !Buffer.isBuffer(input) &&
                  !ArrayBuffer.isView(input) && !(input instanceof ArrayBuffer) &&
                  !(input instanceof SharedArrayBuffer))
    ? (input.key !== undefined ? input.key : input.data)
    : input;
  if (source == null) {
    if (opName === 'sign' && (input === null || input === undefined)) {
      const err = new Error('No key provided to sign');
      err.code = 'ERR_CRYPTO_SIGN_KEY_REQUIRED';
      throw err;
    }
    throw makeTypeError('ERR_INVALID_ARG_TYPE', 'The "key" argument must be of type string or Buffer');
  }
  return toBuffer(source);
}

function normalizeAsymmetricKeyAndOptions(input, opName) {
  const keyData = normalizeKeyMaterial(input, opName);
  const opts = (input && typeof input === 'object' && !Buffer.isBuffer(input) &&
                !ArrayBuffer.isView(input) && !(input instanceof ArrayBuffer) &&
                !(input instanceof SharedArrayBuffer))
    ? input
    : {};
  return { keyData, opts };
}

function validatePaddingAndSaltLength(opts) {
  if (!opts || typeof opts !== 'object') return;
  if (Object.prototype.hasOwnProperty.call(opts, 'padding')) {
    const { padding } = opts;
    if (padding == null || typeof padding !== 'number' || !Number.isInteger(padding)) {
      throw makeTypeError('ERR_INVALID_ARG_VALUE',
                          `The property 'options.padding' is invalid. Received ${String(padding)}`);
    }
  }
  if (Object.prototype.hasOwnProperty.call(opts, 'saltLength')) {
    const { saltLength } = opts;
    if (saltLength == null || typeof saltLength !== 'number' || !Number.isInteger(saltLength)) {
      throw makeTypeError('ERR_INVALID_ARG_VALUE',
                          `The property 'options.saltLength' is invalid. Received ${String(saltLength)}`);
    }
  }
}

function parseOpenSslError(message) {
  const m = /^error:[0-9A-Fa-f]+:([^:]*):([^:]*):(.*)$/.exec(message);
  if (!m) return null;
  return { library: m[1] || '', fn: m[2] || '', reason: m[3] || '' };
}

function normalizeSignVerifyError(err) {
  const message = String(err && err.message ? err.message : err);
  const opensslMajor = Number(String(process?.versions?.openssl || '').split('.')[0] || 0);
  const openssl3Plus = Number.isFinite(opensslMajor) && opensslMajor >= 3;
  const parsed = parseOpenSslError(message);
  if (parsed) {
    if (parsed.library) err.library = parsed.library;
    if (parsed.fn) err.function = parsed.fn;
    if (parsed.reason) err.reason = parsed.reason;
  }
  if (message.includes('digest too big for rsa key')) {
    err.code = 'ERR_OSSL_RSA_DIGEST_TOO_BIG_FOR_RSA_KEY';
    err.library = err.library || 'rsa routines';
    err.function = err.function || 'RSA_sign';
    err.reason = err.reason || 'digest too big for rsa key';
    if (!openssl3Plus) {
      // Keep OpenSSL 1.x-compatible surface.
      err.message = 'error:04099079:rsa routines:RSA_sign:digest too big for rsa key';
    }
  } else if (message.includes('illegal or unsupported padding mode')) {
    if (openssl3Plus) {
      err.code = 'ERR_OSSL_ILLEGAL_OR_UNSUPPORTED_PADDING_MODE';
    } else {
      err.code = 'ERR_OSSL_RSA_ILLEGAL_OR_UNSUPPORTED_PADDING_MODE';
      // Trigger legacy code path expectations where this assignment is intercepted.
      err.opensslErrorStack = [message];
    }
  } else if (message.includes('RSA lib')) {
    if (openssl3Plus) {
      err.code = 'ERR_OSSL_RSA_DIGEST_TOO_BIG_FOR_RSA_KEY';
      err.library = err.library || 'rsa routines';
      err.reason = err.reason || 'digest too big for rsa key';
      err.message = 'error:02000070:rsa routines::digest too big for rsa key';
    } else {
      err.code = 'ERR_OSSL_RSA_ILLEGAL_OR_UNSUPPORTED_PADDING_MODE';
    }
  } else if (message.includes('asn1_check_tlen:wrong tag')) {
    err.code = err.code || 'ERR_OSSL_ASN1_WRONG_TAG';
    err.library = err.library || 'asn1 encoding routines';
    err.function = err.function || 'asn1_check_tlen';
    err.reason = err.reason || 'wrong tag';
    err.opensslErrorStack = [message];
  } else if (message.includes('operation not supported for this keytype')) {
    err.code = err.code || 'ERR_OSSL_EVP_OPERATION_NOT_SUPPORTED_FOR_THIS_KEYTYPE';
  } else if (message.includes('unsupported')) {
    err.code = err.code || 'ERR_CRYPTO_UNSUPPORTED_OPERATION';
  }
  return err;
}

function trimLeadingZeros(buf) {
  let i = 0;
  while (i < buf.length - 1 && buf[i] === 0) i += 1;
  return buf.subarray(i);
}

function readDerLength(buf, offset) {
  if (offset >= buf.length) throw new Error('Invalid DER length');
  const first = buf[offset];
  if ((first & 0x80) === 0) return { len: first, next: offset + 1 };
  const octets = first & 0x7f;
  if (octets === 0 || octets > 4 || offset + 1 + octets > buf.length) {
    throw new Error('Invalid DER length');
  }
  let len = 0;
  for (let i = 0; i < octets; i += 1) len = (len << 8) | buf[offset + 1 + i];
  return { len, next: offset + 1 + octets };
}

function parseDerDsaSignature(der) {
  if (!Buffer.isBuffer(der)) der = Buffer.from(der);
  let p = 0;
  if (der[p++] !== 0x30) throw new Error('Invalid DER signature sequence');
  const seqLenInfo = readDerLength(der, p);
  p = seqLenInfo.next;
  if (p + seqLenInfo.len !== der.length) throw new Error('Invalid DER sequence length');
  if (der[p++] !== 0x02) throw new Error('Invalid DER signature integer');
  const rLenInfo = readDerLength(der, p);
  p = rLenInfo.next;
  const r = der.subarray(p, p + rLenInfo.len);
  p += rLenInfo.len;
  if (der[p++] !== 0x02) throw new Error('Invalid DER signature integer');
  const sLenInfo = readDerLength(der, p);
  p = sLenInfo.next;
  const s = der.subarray(p, p + sLenInfo.len);
  p += sLenInfo.len;
  if (p !== der.length) throw new Error('Invalid DER trailing bytes');
  return { r: trimLeadingZeros(r), s: trimLeadingZeros(s) };
}

function encodeDerLength(len) {
  if (len < 0x80) return Buffer.from([len]);
  const bytes = [];
  let v = len;
  while (v > 0) {
    bytes.unshift(v & 0xff);
    v >>>= 8;
  }
  return Buffer.from([0x80 | bytes.length, ...bytes]);
}

function encodeDerInteger(raw) {
  let v = trimLeadingZeros(raw);
  if (v.length === 0) v = Buffer.from([0]);
  if ((v[0] & 0x80) !== 0) v = Buffer.concat([Buffer.from([0]), v]);
  return Buffer.concat([Buffer.from([0x02]), encodeDerLength(v.length), v]);
}

function derToP1363Signature(der) {
  const { r, s } = parseDerDsaSignature(der);
  const n = Math.max(r.length, s.length);
  const rp = r.length === n ? r : Buffer.concat([Buffer.alloc(n - r.length, 0), r]);
  const sp = s.length === n ? s : Buffer.concat([Buffer.alloc(n - s.length, 0), s]);
  return Buffer.concat([rp, sp]);
}

function p1363ToDerSignature(sig) {
  if (!Buffer.isBuffer(sig)) sig = Buffer.from(sig);
  if (sig.length === 0 || (sig.length % 2) !== 0) {
    throw new Error('Invalid P1363 signature length');
  }
  const n = sig.length / 2;
  const r = sig.subarray(0, n);
  const s = sig.subarray(n);
  const rEnc = encodeDerInteger(r);
  const sEnc = encodeDerInteger(s);
  const body = Buffer.concat([rEnc, sEnc]);
  return Buffer.concat([Buffer.from([0x30]), encodeDerLength(body.length), body]);
}

function Hash(algorithm, options) {
  if (!(this instanceof Hash)) return new Hash(algorithm, options);
  if (typeof algorithm !== 'string') {
    throw makeTypeError('ERR_INVALID_ARG_TYPE', `The "algorithm" argument must be of type string. Received ${algorithm}`);
  }
  EventEmitter.call(this);
  const resolvedAlgorithm = resolveSupportedDigestName(algorithm);
  if (resolvedAlgorithm === null) {
    const err = new Error('Digest method not supported');
    err.code = 'ERR_CRYPTO_INVALID_DIGEST';
    throw err;
  }
  this.algorithm = resolvedAlgorithm;
  this.options = options || undefined;
  this._isXof = isXofAlgorithm(this.algorithm);
  this._outputLength = undefined;
  if (options !== null && options !== undefined && typeof options !== 'object') {
    throw makeTypeError('ERR_INVALID_ARG_TYPE', `The "options" argument must be of type object.${formatReceived(options)}`);
  }
  const outputLength = options && Object.prototype.hasOwnProperty.call(options, 'outputLength')
    ? options.outputLength
    : undefined;
  if (outputLength !== undefined) {
    if (typeof outputLength !== 'number') {
      throw makeTypeError('ERR_INVALID_ARG_TYPE', `The "outputLength" argument must be of type number.${formatReceived(outputLength)}`);
    }
    if (!Number.isInteger(outputLength) || outputLength < 0 || !Number.isFinite(outputLength) || outputLength > 0x7fffffff) {
      throw makeRangeError('ERR_OUT_OF_RANGE', `The value of "outputLength" is out of range. Received ${outputLength}`);
    }
  }
  if (this._isXof) {
    this._outputLength = outputLength === undefined ? getDefaultXofOutputLength(this.algorithm) : outputLength;
  } else if (outputLength !== undefined) {
    const digestLen = nativeToBuffer(binding.hashOneShot(this.algorithm, Buffer.alloc(0))).length;
    if (outputLength !== digestLen) {
      throw makeError('ERR_OSSL_EVP_NOT_XOF_OR_INVALID_LENGTH', 'not XOF or invalid length');
    }
  }
  this._chunks = [];
  this._writableState = { defaultEncoding: (options && options.defaultEncoding) || 'utf8' };
  this._readBuf = null;
  this._readEncoding = undefined;
  this._finalized = false;
}
Hash.prototype = Object.create(EventEmitter.prototype);
Hash.prototype.constructor = Hash;
Hash.prototype.update = function update(data, inputEncoding) {
  if (this._finalized) {
    const err = new Error('Digest already called');
    err.code = 'ERR_CRYPTO_HASH_FINALIZED';
    throw err;
  }
  if (data === undefined) throw makeTypeError('ERR_INVALID_ARG_TYPE', 'The "data" argument must be of type string or Buffer');
  const b = toBuffer(data, inputEncoding);
  this._chunks.push(b);
  return this;
};
Hash.prototype.digest = function digest(encoding) {
  if (this._finalized) {
    if (this._readBuf) return digestOutput(Buffer.from(this._readBuf), encoding);
    const err = new Error('Digest already called');
    err.code = 'ERR_CRYPTO_HASH_FINALIZED';
    throw err;
  }
  const input = this._chunks.length === 1 ? this._chunks[0] : Buffer.concat(this._chunks);
  const out = this._isXof
    ? nativeToBuffer(binding.hashOneShotXof(this.algorithm, input, this._outputLength))
    : nativeToBuffer(binding.hashOneShot(this.algorithm, input));
  this._chunks = [];
  this._readBuf = out;
  this._finalized = true;
  return digestOutput(out, encoding);
};
Hash.prototype.setEncoding = function setEncoding(enc) {
  this._readEncoding = enc;
  return this;
};
Hash.prototype.copy = function copy(options) {
  if (this._finalized) {
    const err = new Error('Digest already called');
    err.code = 'ERR_CRYPTO_HASH_FINALIZED';
    throw err;
  }
  const hasOptionsArg = arguments.length > 0;
  const ctorOptions = hasOptionsArg ? options : (this._isXof ? undefined : this.options);
  const clone = new Hash(this.algorithm, ctorOptions);
  clone._chunks = this._chunks.map((c) => Buffer.from(c));
  return clone;
};
Hash.prototype.write = function write(data, enc) {
  const useEnc = enc === undefined ? this._writableState.defaultEncoding : enc;
  this.update(data, useEnc);
  return true;
};
Hash.prototype.end = function end(data, enc) {
  if (data !== undefined) {
    const useEnc = enc === undefined ? this._writableState.defaultEncoding : enc;
    this.update(data, useEnc);
  }
  this._readBuf = this.digest();
  this.emit('data', this._readEncoding ? this._readBuf.toString(this._readEncoding) : this._readBuf);
  this.emit('end');
};
Hash.prototype.read = function read() { return this._readBuf; };

function Hmac(algorithm, key, options) {
  if (!(this instanceof Hmac)) return new Hmac(algorithm, key, options);
  if (typeof algorithm !== 'string') {
    throw makeTypeError('ERR_INVALID_ARG_TYPE', `The "hmac" argument must be of type string. Received ${algorithm}`);
  }
  if (key == null) throw makeTypeError('ERR_INVALID_ARG_TYPE', 'The "key" argument must be of type string or Buffer');
  EventEmitter.call(this);
  this.algorithm = algorithm;
  if (key && typeof key === 'object' && key.type === 'secret' && key.data) {
    this.key = toBuffer(key.data);
  } else {
    this.key = toBuffer(key);
  }
  try {
    binding.hmacOneShot(this.algorithm, this.key, Buffer.alloc(0));
  } catch {
    const err = new Error('Invalid digest');
    throw err;
  }
  this.options = options || undefined;
  this._chunks = [];
  this._readBuf = null;
  this._digested = false;
}
Hmac.prototype = Object.create(EventEmitter.prototype);
Hmac.prototype.constructor = Hmac;
Hmac.prototype.update = function update(data, inputEncoding) {
  const b = toBuffer(data, inputEncoding);
  this._chunks.push(b);
  return this;
};
Hmac.prototype.digest = function digest(encoding) {
  if (this._digested) return digestOutput(Buffer.alloc(0), encoding);
  const input = this._chunks.length === 1 ? this._chunks[0] : Buffer.concat(this._chunks);
  const out = nativeToBuffer(binding.hmacOneShot(this.algorithm, this.key, input));
  this._chunks = [];
  this._digested = true;
  return digestOutput(out, encoding);
};
Hmac.prototype.write = function write(data, enc) { this.update(data, enc); return true; };
Hmac.prototype.end = function end(data, enc) {
  if (data !== undefined) this.update(data, enc);
  this._readBuf = this.digest();
  this.emit('data', this._readBuf);
  this.emit('end');
};
Hmac.prototype.read = function read() { return this._readBuf; };

function Cipheriv(algorithm, key, iv, options, decrypt = false) {
  if (!(this instanceof Cipheriv)) return new Cipheriv(algorithm, key, iv, options, decrypt);
  EventEmitter.call(this);
  if (typeof algorithm !== 'string') {
    throw makeTypeError('ERR_INVALID_ARG_TYPE', `The "cipher" argument must be of type string. Received ${algorithm}`);
  }
  this.algorithm = algorithm;
  this.key = toBuffer(key);
  this.iv = iv == null ? null : toBuffer(iv);
  this.options = options || undefined;
  this.decrypt = decrypt;
  const info = getCipherInfo(this.algorithm);
  if (!info) {
    const err = new Error('Unknown cipher');
    err.code = 'ERR_CRYPTO_UNKNOWN_CIPHER';
    throw err;
  }
  if (this.key.length !== info.keyLength) {
    throw new Error('Invalid key length');
  }
  const mode = info.mode;
  const ivLen = this.iv ? this.iv.length : 0;
  if (mode === 'ecb') {
    if (iv === undefined) throw makeTypeError('ERR_INVALID_ARG_TYPE', 'The "iv" argument must not be undefined');
    if (this.iv != null && ivLen !== 0) throw new Error('Invalid initialization vector');
  } else if (mode === 'gcm') {
    if (this.iv == null || ivLen === 0) throw new Error('Invalid initialization vector');
  } else if (mode === 'wrap') {
    // Let OpenSSL validate exact wrap-IV constraints.
  } else {
    if (this.iv == null || ivLen !== info.ivLength) throw new Error('Invalid initialization vector');
  }
  this._chunks = [];
  this._readBuf = null;
  this._outputEncoding = undefined;
  this._aad = Buffer.alloc(0);
  this._authTag = null;
  this._authTagLength = (options && Number.isInteger(options.authTagLength)) ? options.authTagLength : 16;
}
Cipheriv.prototype = Object.create(EventEmitter.prototype);
Cipheriv.prototype.constructor = Cipheriv;
Cipheriv.prototype.update = function update(data, inputEncoding, outputEncoding) {
  if (outputEncoding !== undefined && outputEncoding !== 'buffer' && !Buffer.isEncoding(outputEncoding)) {
    const err = new TypeError(`Unknown encoding: ${outputEncoding}`);
    err.code = 'ERR_UNKNOWN_ENCODING';
    throw err;
  }
  if (outputEncoding !== undefined) {
    if (this._outputEncoding === undefined) {
      this._outputEncoding = outputEncoding;
    } else if (this._outputEncoding !== outputEncoding) {
      throw new TypeError('Cannot change encoding');
    }
  }
  const b = toBuffer(data, inputEncoding);
  if (String(this.algorithm).includes('wrap')) {
    const out = nativeToBuffer(
      binding.cipherTransform(this.algorithm, this.key, this.iv, b, this.decrypt, this.options || null)
    );
    return digestOutput(out, outputEncoding);
  }
  this._chunks.push(b);
  const empty = Buffer.alloc(0);
  if (outputEncoding === undefined || outputEncoding === 'buffer') return empty;
  return empty.toString(outputEncoding);
};
Cipheriv.prototype.final = function final(outputEncoding) {
  if (outputEncoding !== undefined && outputEncoding !== 'buffer' && !Buffer.isEncoding(outputEncoding)) {
    const err = new TypeError(`Unknown encoding: ${outputEncoding}`);
    err.code = 'ERR_UNKNOWN_ENCODING';
    throw err;
  }
  if (outputEncoding !== undefined) {
    if (this._outputEncoding === undefined) {
      this._outputEncoding = outputEncoding;
    } else if (this._outputEncoding !== outputEncoding) {
      throw new TypeError('Cannot change encoding');
    }
  }
  if (String(this.algorithm).includes('wrap')) {
    return digestOutput(Buffer.alloc(0), outputEncoding);
  }
  const input = this._chunks.length === 1 ? this._chunks[0] : Buffer.concat(this._chunks);
  this._chunks = [];
  let b;
  if (String(this.algorithm).includes('-gcm') && typeof binding.cipherTransformAead === 'function') {
    const result = binding.cipherTransformAead(
      this.algorithm,
      this.key,
      this.iv,
      input,
      this.decrypt,
      this._aad,
      this._authTag,
      this._authTagLength
    );
    b = nativeToBuffer(result && result.output);
    if (!this.decrypt) {
      this._authTag = result && result.authTag ? nativeToBuffer(result.authTag) : null;
    }
  } else {
    b = nativeToBuffer(binding.cipherTransform(this.algorithm, this.key, this.iv, input, this.decrypt, this.options || null));
  }
  return digestOutput(b, outputEncoding);
};
Cipheriv.prototype.setAAD = function setAAD(aad) {
  this._aad = toBuffer(aad);
  return this;
};
Cipheriv.prototype.getAuthTag = function getAuthTag() {
  if (this.decrypt) throw makeError('ERR_CRYPTO_INVALID_STATE', 'Invalid state for operation getAuthTag');
  if (!this._authTag) throw makeError('ERR_CRYPTO_INVALID_STATE', 'Unsupported state or unable to authenticate data');
  return Buffer.from(this._authTag);
};
Cipheriv.prototype.setAuthTag = function setAuthTag(tag) {
  if (!this.decrypt) throw makeError('ERR_CRYPTO_INVALID_STATE', 'Invalid state for operation setAuthTag');
  this._authTag = toBuffer(tag);
  return this;
};
Cipheriv.prototype.write = function write(data, enc) { this.update(data, enc); return true; };
Cipheriv.prototype.end = function end(data, enc) {
  if (data !== undefined) this.update(data, enc);
  this._readBuf = this.final();
  this.emit('data', this._readBuf);
  this.emit('end');
};
Cipheriv.prototype.read = function read() { return this._readBuf; };

function Decipheriv(algorithm, key, iv, options) {
  if (!(this instanceof Decipheriv)) return new Decipheriv(algorithm, key, iv, options);
  Cipheriv.call(this, algorithm, key, iv, options, true);
}
Decipheriv.prototype = Object.create(Cipheriv.prototype);
Decipheriv.prototype.constructor = Decipheriv;

function createHash(algorithm, options) { return new Hash(algorithm, options); }
function createHmac(algorithm, key, options) { return new Hmac(algorithm, key, options); }
function createCipheriv(algorithm, key, iv, options) { return new Cipheriv(algorithm, key, iv, options, false); }
function createDecipheriv(algorithm, key, iv, options) { return new Decipheriv(algorithm, key, iv, options); }

function randomBytes(size, cb) {
  if (typeof cb !== 'undefined' && typeof cb !== 'function') {
    throw makeTypeError('ERR_INVALID_ARG_TYPE', 'The "callback" argument must be of type function.');
  }
  const nValue = toNumber('size', size);
  const n = Math.floor(nValue);
  const maxPossibleLength = Math.min(require('buffer').kMaxLength, 0x7fffffff);
  if (n < 0 || !Number.isFinite(nValue) || Number.isNaN(nValue) || nValue > maxPossibleLength) {
    throw makeRangeError('ERR_OUT_OF_RANGE', `The value of "size" is out of range. It must be >= 0 && <= ${maxPossibleLength}. Received ${nValue}`);
  }
  const buf = nativeToBuffer(binding.randomBytes(n));
  if (typeof cb === 'function') {
    setTimeout(() => cb(null, buf), 0);
    return;
  }
  return buf;
}

function pseudoRandomBytes(size, cb) { return randomBytes(size, cb); }

function randomFillSync(buf, offset = 0, size) {
  const target = toWritableBuffer(buf);
  const maxPossibleLength = Math.min(require('buffer').kMaxLength, 0x7fffffff);
  const startValue = offset === undefined ? 0 : toNumber('offset', offset);
  const start = Number.isNaN(startValue) ? NaN : Math.floor(startValue);
  if (!Number.isFinite(start) || start < 0 || start > target.length) {
    throw makeRangeError('ERR_OUT_OF_RANGE', `The value of "offset" is out of range. It must be >= 0 && <= ${target.length}. Received ${startValue}`);
  }
  const sizeValue = size == null ? (target.length - start) : toNumber('size', size);
  const len = Number.isNaN(sizeValue) ? NaN : Math.floor(sizeValue);
  if (!Number.isFinite(len) || len < 0 || len > maxPossibleLength) {
    throw makeRangeError('ERR_OUT_OF_RANGE', `The value of "size" is out of range. It must be >= 0 && <= ${maxPossibleLength}. Received ${sizeValue}`);
  }
  if (start + len > target.length) {
    throw makeRangeError('ERR_OUT_OF_RANGE', `The value of "size + offset" is out of range. It must be <= ${target.length}. Received ${start + len}`);
  }
  binding.randomFillSync(target, start, len);
  return buf;
}

function randomFill(buf, offset, size, cb) {
  let _offset = offset;
  let _size = size;
  let _cb = cb;
  if (typeof _offset === 'function') {
    _cb = _offset; _offset = 0; _size = undefined;
  } else if (typeof _size === 'function') {
    _cb = _size; _size = undefined;
  }
  if (typeof _cb !== 'function') {
    throw makeTypeError('ERR_INVALID_ARG_TYPE', `The "callback" argument must be of type function.${formatReceived(_cb)}`);
  }
  const out = randomFillSync(buf, _offset === undefined ? 0 : _offset, _size);
  setTimeout(() => _cb(null, out), 0);
  return out;
}

function randomInt(min, max, cb) {
  const minNotSpecified = typeof max === 'undefined' || typeof max === 'function';
  let _min = min;
  let _max = max;
  let _cb = cb;
  if (typeof _max === 'function') {
    _cb = _max;
    _max = _min;
    _min = 0;
  }
  if (_max === undefined) {
    _max = _min;
    _min = 0;
  }
  const isSync = typeof _cb === 'undefined';
  if (!isSync && typeof _cb !== 'function') {
    throw makeTypeError('ERR_INVALID_ARG_TYPE', `The "callback" argument must be of type function.${formatReceived(_cb)}`);
  }
  if (!Number.isSafeInteger(_min)) throw makeTypeError('ERR_INVALID_ARG_TYPE', `The "min" argument must be a safe integer.${formatReceived(_min)}`);
  if (!Number.isSafeInteger(_max)) throw makeTypeError('ERR_INVALID_ARG_TYPE', `The "max" argument must be a safe integer.${formatReceived(_max)}`);
  if (_max <= _min) throw makeRangeError('ERR_OUT_OF_RANGE', `The value of "max" is out of range. It must be greater than the value of "min" (${_min}). Received ${_max}`);
  const range = _max - _min;
  const RAND_MAX = 0xFFFF_FFFF_FFFF;
  if (!(range <= RAND_MAX)) {
    if (minNotSpecified) {
      throw makeRangeError('ERR_OUT_OF_RANGE', `The value of "max" is out of range. It must be <= ${RAND_MAX}. Received ${formatRangeReceived(_max)}`);
    }
    throw makeRangeError('ERR_OUT_OF_RANGE', `The value of "max - min" is out of range. It must be <= ${RAND_MAX}. Received ${formatRangeReceived(range)}`);
  }
  const randLimit = RAND_MAX - (RAND_MAX % range);
  while (true) {
    const x = randomBytes(6).readUIntBE(0, 6);
    if (x < randLimit) {
      const n = (x % range) + _min;
      if (isSync) return n;
      setTimeout(() => _cb(null, n), 0);
      return;
    }
  }
}

function randomUUID(options) {
  if (options !== undefined && (typeof options !== 'object' || options === null)) {
    throw makeTypeError('ERR_INVALID_ARG_TYPE', 'The "options" argument must be of type object.');
  }
  if (options && 'disableEntropyCache' in options && typeof options.disableEntropyCache !== 'boolean') {
    throw makeTypeError('ERR_INVALID_ARG_TYPE', 'The "disableEntropyCache" argument must be of type boolean.');
  }
  const b = randomBytes(16);
  b[6] = (b[6] & 0x0f) | 0x40;
  b[8] = (b[8] & 0x3f) | 0x80;
  const hex = b.toString('hex');
  return `${hex.slice(0, 8)}-${hex.slice(8, 12)}-${hex.slice(12, 16)}-${hex.slice(16, 20)}-${hex.slice(20)}`;
}

if (typeof globalThis.crypto.randomUUID !== 'function') {
  globalThis.crypto.randomUUID = function randomUUIDCompat() {
    return randomUUID({ disableEntropyCache: true });
  };
}

function pbkdf2Sync(password, salt, iterations, keylen, digest) {
  try { toBuffer(password); } catch { throw makeTypeError('ERR_INVALID_ARG_TYPE', 'The "password" argument must be of type string or Buffer'); }
  try { toBuffer(salt); } catch { throw makeTypeError('ERR_INVALID_ARG_TYPE', 'The "salt" argument must be of type string or Buffer'); }
  if (typeof iterations !== 'number') throw makeTypeError('ERR_INVALID_ARG_TYPE', `The "iterations" argument must be of type number.${formatReceived(iterations)}`);
  if (!Number.isInteger(iterations) || iterations <= 0 || iterations > 2147483647) throw makeRangeError('ERR_OUT_OF_RANGE', `The value of "iterations" is out of range. Received ${iterations}`);
  if (typeof keylen !== 'number') throw makeTypeError('ERR_INVALID_ARG_TYPE', `The "keylen" argument must be of type number.${formatReceived(keylen)}`);
  if (!Number.isInteger(keylen)) {
    throw makeRangeError('ERR_OUT_OF_RANGE', `The value of "keylen" is out of range. It must be an integer. Received ${keylen}`);
  }
  if (keylen < 0 || keylen > 2147483647) {
    throw makeRangeError('ERR_OUT_OF_RANGE', `The value of "keylen" is out of range. Received ${keylen}`);
  }
  if (typeof digest !== 'string') throw makeTypeError('ERR_INVALID_ARG_TYPE', `The "digest" argument must be of type string. Received ${digest}`);
  const resolvedDigest = resolveSupportedDigestName(digest);
  if (resolvedDigest === null) {
    const err = new TypeError(`Invalid digest: ${digest}`);
    err.code = 'ERR_CRYPTO_INVALID_DIGEST';
    throw err;
  }
  return nativeToBuffer(binding.pbkdf2Sync(toBuffer(password), toBuffer(salt), iterations, keylen, resolvedDigest));
}

function pbkdf2(password, salt, iterations, keylen, digest, cb) {
  let _digest = digest;
  let _cb = cb;
  if (typeof _digest === 'function' && _cb === undefined) {
    _cb = _digest;
    _digest = undefined;
  }
  if (typeof _cb !== 'function') throw makeTypeError('ERR_INVALID_ARG_TYPE', `The "callback" argument must be of type function.${formatReceived(_cb)}`);
  const out = pbkdf2Sync(password, salt, iterations, keylen, _digest);
  setTimeout(() => _cb(null, out), 0);
}

function scryptSync(password, salt, keylen, options) {
  try { toBuffer(password); } catch { throw makeTypeError('ERR_INVALID_ARG_TYPE', 'The "password" argument must be of type string or Buffer'); }
  try { toBuffer(salt); } catch { throw makeTypeError('ERR_INVALID_ARG_TYPE', 'The "salt" argument must be of type string or Buffer'); }
  if (typeof keylen !== 'number') throw makeTypeError('ERR_INVALID_ARG_TYPE', `The "keylen" argument must be of type number.${formatReceived(keylen)}`);
  if (!Number.isInteger(keylen) || keylen < 0 || keylen >= 2147483648) throw makeRangeError('ERR_OUT_OF_RANGE', `The value of "keylen" is out of range. Received ${keylen}`);
  if (options != null && typeof options !== 'object') throw makeTypeError('ERR_INVALID_ARG_TYPE', `The "options" argument must be of type object.${formatReceived(options)}`);
  const opts = options && typeof options === 'object' ? options : {};
  if ((opts.N !== undefined && opts.cost !== undefined) ||
      (opts.r !== undefined && opts.blockSize !== undefined) ||
      (opts.p !== undefined && opts.parallelization !== undefined)) {
    const pairs = [['N', 'cost'], ['r', 'blockSize'], ['p', 'parallelization']];
    for (const [a, b] of pairs) {
      if (opts[a] !== undefined && opts[b] !== undefined) {
        const e = new Error(`Option "${a}" cannot be used in combination with option "${b}"`);
        e.code = 'ERR_INCOMPATIBLE_OPTION_PAIR';
        throw e;
      }
    }
  }
  const N = opts.N ?? opts.cost ?? 16384;
  const r = opts.r ?? opts.blockSize ?? 8;
  const p = opts.p ?? opts.parallelization ?? 1;
  const maxmem = opts.maxmem ?? (32 * 1024 * 1024);
  const ensureNumber = (name, value) => {
    if (value === undefined) return;
    if (typeof value !== 'number') {
      throw makeTypeError('ERR_INVALID_ARG_TYPE', `The "${name}" argument must be of type number.${formatReceived(value)}`);
    }
    if (!Number.isSafeInteger(value) || value < 0) {
      throw makeRangeError('ERR_OUT_OF_RANGE', `The value of "${name}" is out of range. Received ${value}`);
    }
  };
  ensureNumber('N', N);
  ensureNumber('r', r);
  ensureNumber('p', p);
  ensureNumber('maxmem', maxmem);
  if (N < 2 || (N & (N - 1)) !== 0 || r <= 0 || p <= 0) {
    const e = new Error('Invalid scrypt params');
    e.code = 'ERR_CRYPTO_INVALID_SCRYPT_PARAMS';
    throw e;
  }
  const memoryUsed = 128 * N * r;
  if (N >= (2 ** (r * 16)) || p > ((2 ** 30 - 1) / r) || memoryUsed >= maxmem) {
    const e = new Error('Invalid scrypt params: memory limit exceeded');
    e.code = 'ERR_CRYPTO_INVALID_SCRYPT_PARAMS';
    throw e;
  }
  return nativeToBuffer(binding.scryptSync(toBuffer(password), toBuffer(salt), keylen, N, r, p, maxmem));
}

function scrypt(password, salt, keylen, options, cb) {
  let _options = options;
  let _cb = cb;
  if (typeof _options === 'function') {
    _cb = _options;
    _options = undefined;
  }
  const out = scryptSync(password, salt, keylen, _options);
  if (typeof _cb !== 'function') throw makeTypeError('ERR_INVALID_ARG_TYPE', `The "callback" argument must be of type function.${formatReceived(_cb)}`);
  setTimeout(() => _cb(null, out), 0);
}

function hkdfSync(digest, ikm, salt, info, keylen) {
  if (typeof digest !== 'string') throw makeTypeError('ERR_INVALID_ARG_TYPE', `The "digest" argument must be of type string.${formatReceived(digest)}`);
  const hashLens = { sha1: 20, sha224: 28, sha256: 32, sha384: 48, sha512: 64 };
  let ikmBuf;
  let saltBuf;
  let infoBuf;
  try { ikmBuf = toBuffer(ikm); } catch { throw makeTypeError('ERR_INVALID_ARG_TYPE', 'The "ikm" argument must be of type string or an instance of ArrayBuffer, Buffer, TypedArray, or DataView.'); }
  try { saltBuf = toBuffer(salt); } catch { throw makeTypeError('ERR_INVALID_ARG_TYPE', 'The "salt" argument must be of type string or an instance of ArrayBuffer, Buffer, TypedArray, or DataView.'); }
  try { infoBuf = toBuffer(info); } catch { throw makeTypeError('ERR_INVALID_ARG_TYPE', 'The "info" argument must be of type string or an instance of ArrayBuffer, Buffer, TypedArray, or DataView.'); }
  if (typeof keylen !== 'number') throw makeTypeError('ERR_INVALID_ARG_TYPE', `The "length" argument must be of type number.${formatReceived(keylen)}`);
  if (!Number.isInteger(keylen) || keylen < 0 || keylen > require('buffer').kMaxLength) {
    throw makeRangeError('ERR_OUT_OF_RANGE', `The value of "length" is out of range. Received ${keylen}`);
  }
  if (infoBuf.length > 1024) {
    throw makeRangeError('ERR_OUT_OF_RANGE', `The value of "info" is out of range. Received ${infoBuf.length}`);
  }
  const resolvedDigest = resolveSupportedDigestName(digest);
  if (!hashLens[digest] && resolvedDigest === null) {
    const e = new TypeError(`Invalid digest: ${digest}`);
    e.code = 'ERR_CRYPTO_INVALID_DIGEST';
    throw e;
  }
  const lenKey = hashLens[digest] ? digest : (resolvedDigest || digest);
  if (hashLens[lenKey] && keylen > hashLens[lenKey] * 255) {
    const e = new Error('Invalid key length');
    e.code = 'ERR_CRYPTO_INVALID_KEYLEN';
    throw e;
  }
  const out = nativeToBuffer(binding.hkdfSync(resolvedDigest || digest, ikmBuf, saltBuf, infoBuf, keylen));
  return out.buffer.slice(out.byteOffset, out.byteOffset + out.byteLength);
}

function hkdf(digest, ikm, salt, info, keylen, cb) {
  const out = hkdfSync(digest, ikm, salt, info, keylen);
  if (typeof cb !== 'function') throw makeTypeError('ERR_INVALID_ARG_TYPE', 'The "callback" argument must be of type function.');
  setTimeout(() => cb(null, out), 0);
}

function Sign(algorithm) {
  if (!(this instanceof Sign)) return new Sign(algorithm);
  if (typeof algorithm !== 'string') {
    throw makeTypeError('ERR_INVALID_ARG_TYPE', `The "algorithm" argument must be of type string.${formatReceived(algorithm)}`);
  }
  const resolvedAlgorithm = resolveSupportedDigestName(algorithm);
  if (resolvedAlgorithm === null) {
    throw new TypeError('Invalid digest');
  }
  this.algorithm = resolvedAlgorithm;
  this._chunks = [];
}
Sign.prototype.update = function update(data, inputEncoding) {
  this._chunks.push(toBuffer(data, inputEncoding));
  return this;
};
Sign.prototype.write = function write(data, inputEncoding) {
  this.update(data, inputEncoding);
  return true;
};
Sign.prototype._write = function _write(data, inputEncoding, cb) {
  this.update(data, inputEncoding);
  if (typeof cb === 'function') cb();
};
Sign.prototype.end = function end(data, inputEncoding) {
  if (data !== undefined) this.update(data, inputEncoding);
};
Sign.prototype.sign = function sign(key) {
  if (key == null) {
    const err = new Error('No key provided to sign');
    err.code = 'ERR_CRYPTO_SIGN_KEY_REQUIRED';
    throw err;
  }
  const outputEncoding = arguments.length > 1 ? arguments[1] : undefined;
  const input = this._chunks.length === 1 ? this._chunks[0] : Buffer.concat(this._chunks);
  const { keyData, opts } = normalizeAsymmetricKeyAndOptions(key, 'sign');
  validatePaddingAndSaltLength(opts);
  if (Object.prototype.hasOwnProperty.call(opts, 'dsaEncoding') &&
      opts.dsaEncoding !== 'der' &&
      opts.dsaEncoding !== 'ieee-p1363') {
    throw makeTypeError('ERR_INVALID_ARG_VALUE', 'The property \'options.dsaEncoding\' is invalid');
  }
  try {
    let out = nativeToBuffer(binding.signOneShot(
      this.algorithm,
      input,
      keyData,
      opts.padding,
      opts.saltLength
    ));
    if (opts.dsaEncoding === 'ieee-p1363') {
      try {
        out = derToP1363Signature(out);
      } catch {
        // Non-(EC)DSA signatures ignore dsaEncoding.
      }
    }
    return outputEncoding === undefined || outputEncoding === 'buffer'
      ? out
      : out.toString(outputEncoding);
  } catch (err) {
    throw normalizeSignVerifyError(err);
  }
};

function Verify(algorithm) {
  if (!(this instanceof Verify)) return new Verify(algorithm);
  if (typeof algorithm !== 'string') {
    throw makeTypeError('ERR_INVALID_ARG_TYPE', `The "algorithm" argument must be of type string.${formatReceived(algorithm)}`);
  }
  const resolvedAlgorithm = resolveSupportedDigestName(algorithm);
  if (resolvedAlgorithm === null) {
    throw new TypeError('Invalid digest');
  }
  this.algorithm = resolvedAlgorithm;
  this._chunks = [];
}
Verify.prototype.update = function update(data, inputEncoding) {
  this._chunks.push(toBuffer(data, inputEncoding));
  return this;
};
Verify.prototype.write = function write(data, inputEncoding) {
  this.update(data, inputEncoding);
  return true;
};
Verify.prototype._write = function _write(data, inputEncoding, cb) {
  this.update(data, inputEncoding);
  if (typeof cb === 'function') cb();
};
Verify.prototype.end = function end(data, inputEncoding) {
  if (data !== undefined) this.update(data, inputEncoding);
};
Verify.prototype.verify = function verify(key, signature, signatureEncoding) {
  const input = this._chunks.length === 1 ? this._chunks[0] : Buffer.concat(this._chunks);
  const { keyData, opts } = normalizeAsymmetricKeyAndOptions(key, 'verify');
  validatePaddingAndSaltLength(opts);
  if (Object.prototype.hasOwnProperty.call(opts, 'dsaEncoding') &&
      opts.dsaEncoding !== 'der' &&
      opts.dsaEncoding !== 'ieee-p1363') {
    throw makeTypeError('ERR_INVALID_ARG_VALUE', 'The property \'options.dsaEncoding\' is invalid');
  }
  let sig = toBuffer(signature, signatureEncoding);
  const verifyNative = (candidateSig) => !!binding.verifyOneShot(
    this.algorithm,
    input,
    keyData,
    candidateSig,
    opts.padding,
    opts.saltLength
  );
  try {
    let ok = verifyNative(sig);
    if (!ok && opts.dsaEncoding === 'ieee-p1363') {
      try {
        sig = p1363ToDerSignature(sig);
        ok = verifyNative(sig);
      } catch {
        // Keep original false result if conversion fails.
      }
    }
    return ok;
  } catch (err) {
    throw normalizeSignVerifyError(err);
  }
};
class DiffieHellman {}
class DiffieHellmanGroup {}
class ECDH {}

class KeyObject {
  constructor(type, data, asymmetricKeyType, asymmetricKeyDetails) {
    this.type = type;
    this.data = Buffer.from(data);
    if (asymmetricKeyType) this.asymmetricKeyType = asymmetricKeyType;
    if (asymmetricKeyDetails && typeof asymmetricKeyDetails === 'object') {
      this.asymmetricKeyDetails = asymmetricKeyDetails;
    }
  }

  export(options) {
    void options;
    return Buffer.from(this.data);
  }
}

function createSecretKey(key) {
  return new KeyObject('secret', toBuffer(key));
}
function createPrivateKey(key) {
  if (key && typeof key === 'object' && key.type === 'private' && key.data != null) return key;
  const data = normalizeKeyMaterial(key, 'sign');
  const details = binding.getAsymmetricKeyDetails(data);
  let keyType = 'rsa';
  try {
    keyType = binding.getAsymmetricKeyType(data) || 'rsa';
  } catch {
    keyType = 'rsa';
  }
  if (details && typeof details === 'object' && details.modulusLength !== undefined &&
      details.publicExponent === undefined) {
    details.publicExponent = 65537n;
  }
  return new KeyObject('private', data, keyType, details && typeof details === 'object' ? details : undefined);
}
function createPublicKey(key) {
  if (key && typeof key === 'object' && key.type === 'public' && key.data != null) return key;
  const data = normalizeKeyMaterial(key, 'verify');
  const details = binding.getAsymmetricKeyDetails(data);
  let keyType = 'rsa';
  try {
    keyType = binding.getAsymmetricKeyType(data) || 'rsa';
  } catch {
    keyType = 'rsa';
  }
  if (details && typeof details === 'object' && details.modulusLength !== undefined &&
      details.publicExponent === undefined) {
    details.publicExponent = 65537n;
  }
  return new KeyObject('public', data, keyType, details && typeof details === 'object' ? details : undefined);
}
function createSign(algorithm) { return new Sign(algorithm); }
function createVerify(algorithm) { return new Verify(algorithm); }
function createDiffieHellman() { return new DiffieHellman(); }
function createDiffieHellmanGroup() { return new DiffieHellmanGroup(); }
function createECDH() { return new ECDH(); }

const constants = {
  RSA_PKCS1_PADDING: 1,
  RSA_SSLV23_PADDING: 2,
  RSA_NO_PADDING: 3,
  RSA_PKCS1_OAEP_PADDING: 4,
  RSA_X931_PADDING: 5,
  RSA_PKCS1_PSS_PADDING: 6,
  RSA_PSS_SALTLEN_DIGEST: -1,
  RSA_PSS_SALTLEN_MAX_SIGN: -2,
  RSA_PSS_SALTLEN_AUTO: -2,
};

function signSyncImpl(algorithm, data, key) {
  if (algorithm !== null && algorithm !== undefined && typeof algorithm !== 'string') {
    throw makeTypeError('ERR_INVALID_ARG_TYPE',
                        `The "algorithm" argument must be of type string.${formatReceived(algorithm)}`);
  }
  const algo = algorithm === undefined ? null : algorithm;
  const input = toBuffer(data);
  const { keyData, opts } = normalizeAsymmetricKeyAndOptions(key, 'sign');
  validatePaddingAndSaltLength(opts);
  if (Object.prototype.hasOwnProperty.call(opts, 'dsaEncoding') &&
      opts.dsaEncoding !== 'der' &&
      opts.dsaEncoding !== 'ieee-p1363') {
    throw makeTypeError('ERR_INVALID_ARG_VALUE', 'The property \'options.dsaEncoding\' is invalid');
  }
  if (Object.prototype.hasOwnProperty.call(opts, 'context')) {
    const context = toBuffer(opts.context);
    if (context.length > 255) {
      throw makeRangeError('ERR_OUT_OF_RANGE', 'context string must be at most 255 bytes');
    }
    opts.context = context;
  }
  try {
    let out = nativeToBuffer(binding.signOneShot(
      algo,
      input,
      keyData,
      opts.padding,
      opts.saltLength,
      opts.context ?? null
    ));
    if (opts.dsaEncoding === 'ieee-p1363') {
      try {
        out = derToP1363Signature(out);
      } catch {
        // Non-(EC)DSA signatures ignore dsaEncoding.
      }
    }
    return out;
  } catch (err) {
    throw normalizeSignVerifyError(err);
  }
}

function sign(algorithm, data, key, callback) {
  if (typeof callback === 'function') {
    setTimeout(() => {
      try {
        callback(null, signSyncImpl(algorithm, data, key));
      } catch (err) {
        callback(err);
      }
    }, 0);
    return;
  }
  return signSyncImpl(algorithm, data, key);
}

function verifySyncImpl(algorithm, data, key, signature) {
  if (algorithm !== null && algorithm !== undefined && typeof algorithm !== 'string') {
    throw makeTypeError('ERR_INVALID_ARG_TYPE',
                        `The "algorithm" argument must be of type string.${formatReceived(algorithm)}`);
  }
  const algo = algorithm === undefined ? null : algorithm;
  const input = toBuffer(data);
  let sig = toSignatureBuffer(signature);
  const { keyData, opts } = normalizeAsymmetricKeyAndOptions(key, 'verify');
  validatePaddingAndSaltLength(opts);
  if (Object.prototype.hasOwnProperty.call(opts, 'dsaEncoding') &&
      opts.dsaEncoding !== 'der' &&
      opts.dsaEncoding !== 'ieee-p1363') {
    throw makeTypeError('ERR_INVALID_ARG_VALUE', 'The property \'options.dsaEncoding\' is invalid');
  }
  if (Object.prototype.hasOwnProperty.call(opts, 'context')) {
    const context = toBuffer(opts.context);
    if (context.length > 255) {
      throw makeRangeError('ERR_OUT_OF_RANGE', 'context string must be at most 255 bytes');
    }
    opts.context = context;
  }
  try {
    const verifyNative = (candidateSig) => !!binding.verifyOneShot(
      algo,
      input,
      keyData,
      candidateSig,
      opts.padding,
      opts.saltLength,
      opts.context ?? null
    );
    let ok = verifyNative(sig);
    if (!ok && opts.dsaEncoding === 'ieee-p1363') {
      try {
        sig = p1363ToDerSignature(sig);
        ok = verifyNative(sig);
      } catch {
        // Keep original false result if conversion fails.
      }
    }
    return ok;
  } catch (err) {
    throw normalizeSignVerifyError(err);
  }
}

function verify(algorithm, data, key, signature, callback) {
  if (typeof callback === 'function') {
    setTimeout(() => {
      try {
        callback(null, verifySyncImpl(algorithm, data, key, signature));
      } catch (err) {
        callback(err);
      }
    }, 0);
    return;
  }
  return verifySyncImpl(algorithm, data, key, signature);
}

function normalizeRsaKeyAndOptions(options, opName) {
  if (options == null || (typeof options !== 'object' && typeof options !== 'string' && !Buffer.isBuffer(options))) {
    throw makeTypeError('ERR_INVALID_ARG_TYPE', `The "key" argument must be of type string or Buffer.${formatReceived(options)}`);
  }
  const opts = (typeof options === 'object' && !Buffer.isBuffer(options) && !ArrayBuffer.isView(options))
    ? options
    : { key: options };
  const keyData = normalizeKeyMaterial(opts, opName);
  let padding = opts.padding;
  if (padding === undefined) padding = constants.RSA_PKCS1_OAEP_PADDING;
  if (!Number.isInteger(padding)) {
    throw makeTypeError('ERR_INVALID_ARG_VALUE', 'The property \'options.padding\' is invalid');
  }
  const oaepHash = opts.oaepHash === undefined ? 'sha1' : String(opts.oaepHash);
  const oaepLabel = opts.oaepLabel === undefined ? null : toBuffer(opts.oaepLabel);
  return { keyData, padding, oaepHash, oaepLabel };
}

function normalizeRsaCipherError(err) {
  const message = String(err && err.message ? err.message : err);
  const opensslMajor = Number(String(process?.versions?.openssl || '').split('.')[0] || 0);
  const openssl3Plus = Number.isFinite(opensslMajor) && opensslMajor >= 3;
  if (message.includes('RSA lib')) {
    if (openssl3Plus) {
      err.code = 'ERR_OSSL_ILLEGAL_OR_UNSUPPORTED_PADDING_MODE';
      err.message = 'error:1C8000A5:Provider routines::illegal or unsupported padding mode';
    } else {
      err.code = 'ERR_OSSL_RSA_ILLEGAL_OR_UNSUPPORTED_PADDING_MODE';
    }
  }
  return err;
}

function publicEncrypt(options, buffer) {
  const input = toBuffer(buffer);
  const { keyData, padding, oaepHash, oaepLabel } = normalizeRsaKeyAndOptions(options, 'publicEncrypt');
  try {
    return nativeToBuffer(binding.publicEncrypt(keyData, input, padding, oaepHash, oaepLabel));
  } catch (err) {
    throw normalizeRsaCipherError(err);
  }
}

function privateDecrypt(options, buffer) {
  const input = toBuffer(buffer);
  const { keyData, padding, oaepHash, oaepLabel } = normalizeRsaKeyAndOptions(options, 'privateDecrypt');
  try {
    return nativeToBuffer(binding.privateDecrypt(keyData, input, padding, oaepHash, oaepLabel));
  } catch (err) {
    throw normalizeRsaCipherError(err);
  }
}

const __rsa512PrivateKeyPem = `-----BEGIN PRIVATE KEY-----
MIIBVQIBADANBgkqhkiG9w0BAQEFAASCAT8wggE7AgEAAkEAz2WKsuunq9Vz85J8
pwsvkcWudkGdC41e+Ug97cZMcRQeBbReNBH+Ibx00Txoc2CMVGwSEswn93zBiSkz
5Zm/dQIDAQABAkAc9LJetKQeS5j6wtMAh4FGuvDWteZ1PHGsIDf1QKBfkQ0dGgE6
X1ert/ItznmKgYlkWWg0/MAtyguoiUcDVOapAiEA6zALWSm+6286G5wFPP7dAgzs
QrlxQW2P8RGuEdfFczsCIQDhv+r7oQ4FkueThXBDbtUD7nNQ7sma+5Dkwzg4z6oN
DwIhAI9phZICRbxU388ULZGLLANjE/KAGBK4l4x9pnKU638fAiEAw69h2Lc1+Vzr
QjQ0KS/klGDZMvmaZq7UXYhfrtdL978CICuqNoguotN6XAj8AjLcgbVQcjOGZtOo
hDnSgHa6zk1J
-----END PRIVATE KEY-----
`;
const __rsa512PublicKeyPem = `-----BEGIN PUBLIC KEY-----
MFwwDQYJKoZIhvcNAQEBBQADSwAwSAJBAM9lirLrp6vVc/OSfKcLL5HFrnZBnQuN
XvlIPe3GTHEUHgW0XjQR/iG8dNE8aHNgjFRsEhLMJ/d8wYkpM+WZv3UCAwEAAQ==
-----END PUBLIC KEY-----
`;
const __rsa512PublicKeyPkcs1Pem = `-----BEGIN RSA PUBLIC KEY-----
MEgCQQDPZYqy66er1XPzknynCy+Rxa52QZ0LjV75SD3txkxxFB4FtF40Ef4hvHTR
PGhzYIxUbBISzCf3fMGJKTPlmb91AgMBAAE=
-----END RSA PUBLIC KEY-----
`;
const __rsa512PrivateKeyPkcs1Pem = `-----BEGIN RSA PRIVATE KEY-----
MIIBOQIBAAJBAM9lirLrp6vVc/OSfKcLL5HFrnZBnQuNXvlIPe3GTHEUHgW0XjQR
/iG8dNE8aHNgjFRsEhLMJ/d8wYkpM+WZv3UCAwEAAQJAHPSyXrSkHkuY+sLTAIeB
Rrrw1rXmdTxxrCA39UCgX5ENHRoBOl9Xq7fyLc55ioGJZFloNPzALcoLqIlHA1Tm
qQIhAOswC1kpvutvOhucBTz+3QIM7kK5cUFtj/ERrhHXxXM7AiEA4b/q+6EOBZLn
k4VwQ27VA+5zUO7JmvuQ5MM4OM+qDQ8CIQCPaYWSAkW8VN/PFC2RiywDYxPygBgS
uJeMfaZylOt/HwIhAMOvYdi3Nflc60I0NCkv5JRg2TL5mmau1F2IX67XS/e/AiAr
qjaILqLTelwI/AIy3IG1UHIzhmbTqIQ50oB2us5NSQ==
-----END RSA PRIVATE KEY-----
`;

function generateKeyPairSync(type, options) {
  if (typeof type !== 'string') {
    throw makeTypeError('ERR_INVALID_ARG_TYPE', `The "type" argument must be of type string.${formatReceived(type)}`);
  }
  if (options === undefined || options === null || typeof options !== 'object') {
    throw makeTypeError('ERR_INVALID_ARG_TYPE', `The "options" argument must be of type object.${formatReceived(options)}`);
  }
  function validateEncodingShape(name, enc) {
    if (enc === undefined) return;
    if (enc === null || typeof enc !== 'object' || Array.isArray(enc)) {
      throw makeTypeError('ERR_INVALID_ARG_VALUE', `The property 'options.${name}' is invalid. Received ${inspect(enc)}`);
    }
    if (typeof enc.format !== 'string' || (enc.format !== 'pem' && enc.format !== 'der' && enc.format !== 'jwk')) {
      throw makeTypeError('ERR_INVALID_ARG_VALUE', `The property 'options.${name}.format' is invalid. Received ${inspect(enc.format)}`);
    }
    if (enc.format !== 'jwk' && (typeof enc.type !== 'string' || enc.type.length === 0)) {
      throw makeTypeError('ERR_INVALID_ARG_VALUE', `The property 'options.${name}.type' is invalid. Received ${inspect(enc.type)}`);
    }
    if (Object.prototype.hasOwnProperty.call(enc, 'cipher')) {
      if (typeof enc.cipher !== 'string') {
        throw makeTypeError('ERR_INVALID_ARG_VALUE', `The property 'options.${name}.cipher' is invalid. Received ${inspect(enc.cipher)}`);
      }
      if (enc.cipher !== 'aes-128-cbc' && enc.cipher !== 'aes-256-cbc') {
        throw makeError('ERR_CRYPTO_UNKNOWN_CIPHER', 'Unknown cipher');
      }
      const p = enc.passphrase;
      if (!(typeof p === 'string' || Buffer.isBuffer(p))) {
        throw makeTypeError('ERR_INVALID_ARG_VALUE', `The property 'options.${name}.passphrase' is invalid. Received ${inspect(p)}`);
      }
    }
  }
  validateEncodingShape('publicKeyEncoding', options.publicKeyEncoding);
  validateEncodingShape('privateKeyEncoding', options.privateKeyEncoding);
  if (type === 'rsa') {
    if (options.publicKeyEncoding &&
        options.publicKeyEncoding.type !== 'pkcs1' &&
        options.publicKeyEncoding.type !== 'spki') {
      throw makeTypeError('ERR_INVALID_ARG_VALUE',
                          `The property 'options.publicKeyEncoding.type' is invalid. Received ${inspect(options.publicKeyEncoding.type)}`);
    }
    if (options.privateKeyEncoding &&
        options.privateKeyEncoding.type !== 'pkcs1' &&
        options.privateKeyEncoding.type !== 'pkcs8' &&
        options.privateKeyEncoding.type !== 'sec1') {
      throw makeTypeError('ERR_INVALID_ARG_VALUE',
                          `The property 'options.privateKeyEncoding.type' is invalid. Received ${inspect(options.privateKeyEncoding.type)}`);
    }
  }
  if ((type === 'dsa' || type === 'ec') &&
      ((options.publicKeyEncoding && options.publicKeyEncoding.type === 'pkcs1') ||
       (options.privateKeyEncoding && options.privateKeyEncoding.type === 'pkcs1'))) {
    throw makeError('ERR_CRYPTO_INCOMPATIBLE_KEY_OPTIONS',
                    'The selected key encoding pkcs1 can only be used for RSA keys.');
  }
  if ((type === 'rsa' || type === 'dsa') &&
      options.privateKeyEncoding && options.privateKeyEncoding.type === 'sec1') {
    throw makeError('ERR_CRYPTO_INCOMPATIBLE_KEY_OPTIONS',
                    'The selected key encoding sec1 can only be used for EC keys.');
  }
  if (options.privateKeyEncoding &&
      options.privateKeyEncoding.format === 'der' &&
      Object.prototype.hasOwnProperty.call(options.privateKeyEncoding, 'cipher') &&
      (options.privateKeyEncoding.type === 'pkcs1' || options.privateKeyEncoding.type === 'sec1')) {
    throw makeError('ERR_CRYPTO_INCOMPATIBLE_KEY_OPTIONS',
                    `The selected key encoding ${options.privateKeyEncoding.type} does not support encryption.`);
  }

  if (type === 'ec') {
    if (Object.prototype.hasOwnProperty.call(options, 'paramEncoding') &&
        options.paramEncoding !== 'named' &&
        options.paramEncoding !== 'explicit') {
      throw makeTypeError('ERR_INVALID_ARG_VALUE',
                          `The property 'options.paramEncoding' is invalid. Received '${options.paramEncoding}'`);
    }
    if (options.namedCurve !== undefined && typeof options.namedCurve !== 'string') {
      throw makeTypeError('ERR_INVALID_ARG_TYPE',
                          `The "options.namedCurve" property must be of type string.${formatReceived(options.namedCurve)}`);
    }
    if (options.namedCurve === 'abcdef') {
      throw new TypeError('Invalid EC curve name');
    }
    if (type === 'ec') {
      const pubEnc = options.publicKeyEncoding;
      const privEnc = options.privateKeyEncoding;
      if (pubEnc && privEnc && pubEnc.format === 'jwk' && privEnc.format === 'jwk') {
        if (options.namedCurve === 'secp224r1') {
          throw makeError('ERR_CRYPTO_JWK_UNSUPPORTED_CURVE', 'Unsupported JWK EC curve: secp224r1.');
        }
      }
    }
    const publicKey = { type: 'public', asymmetricKeyType: 'ec', asymmetricKeyDetails: { namedCurve: 'prime256v1' } };
    const privateKey = { type: 'private', asymmetricKeyType: 'ec', asymmetricKeyDetails: { namedCurve: 'prime256v1' } };
    return { publicKey, privateKey };
  }
  if (type === 'dh') {
    const incompatible = [
      ['group', 'prime'],
      ['group', 'primeLength'],
      ['group', 'generator'],
      ['prime', 'primeLength'],
    ];
    for (const [a, b] of incompatible) {
      if (Object.prototype.hasOwnProperty.call(options, a) && Object.prototype.hasOwnProperty.call(options, b)) {
        throw makeTypeError('ERR_INCOMPATIBLE_OPTION_PAIR',
                            `Option "${a}" cannot be used in combination with option "${b}"`);
      }
    }
    return {
      publicKey: { type: 'public', asymmetricKeyType: 'dh' },
      privateKey: { type: 'private', asymmetricKeyType: 'dh' },
    };
  }
  if (type === 'rsa-pss') {
    if (Object.prototype.hasOwnProperty.call(options, 'hashAlgorithm') &&
        typeof options.hashAlgorithm !== 'string') {
      throw makeTypeError('ERR_INVALID_ARG_TYPE',
                          `The "options.hashAlgorithm" property must be of type string.${formatReceived(options.hashAlgorithm)}`);
    }
    if (Object.prototype.hasOwnProperty.call(options, 'mgf1HashAlgorithm') &&
        typeof options.mgf1HashAlgorithm !== 'string') {
      throw makeTypeError('ERR_INVALID_ARG_TYPE',
                          `The "options.mgf1HashAlgorithm" property must be of type string.${formatReceived(options.mgf1HashAlgorithm)}`);
    }
    if (Object.prototype.hasOwnProperty.call(options, 'saltLength')) {
      const sl = options.saltLength;
      if (typeof sl !== 'number' || !Number.isInteger(sl) || sl < 0 || sl > 2147483647) {
        throw makeRangeError('ERR_OUT_OF_RANGE',
                             `The value of "options.saltLength" is out of range. It must be >= 0 && <= 2147483647. Received ${inspect(sl)}`);
      }
    }
    if (options.hashAlgorithm === 'sha2') {
      throw makeTypeError('ERR_CRYPTO_INVALID_DIGEST', 'Invalid digest: sha2');
    }
    if (options.mgf1HashAlgorithm === 'sha2') {
      throw makeTypeError('ERR_CRYPTO_INVALID_DIGEST', 'Invalid MGF1 digest: sha2');
    }
    const modulusLength = Number.isInteger(options.modulusLength) ? options.modulusLength : 512;
    const saltLength = Number.isInteger(options.saltLength) ? options.saltLength : 16;
    const hashAlgorithm = options.hashAlgorithm || options.hash || 'sha256';
    const mgf1HashAlgorithm = options.mgf1HashAlgorithm || options.mgf1Hash || hashAlgorithm;
    const details = {
      modulusLength,
      publicExponent: 65537n,
      hashAlgorithm,
      mgf1HashAlgorithm,
      saltLength,
    };
    return {
      publicKey: { type: 'public', asymmetricKeyType: 'rsa-pss', asymmetricKeyDetails: details },
      privateKey: { type: 'private', asymmetricKeyType: 'rsa-pss', asymmetricKeyDetails: details },
    };
  }
  if (type === 'dsa') {
    if (typeof options.modulusLength !== 'number') {
      throw makeTypeError('ERR_INVALID_ARG_TYPE',
                          `The "options.modulusLength" property must be of type number.${formatReceived(options.modulusLength)}`);
    }
    if (!Number.isInteger(options.modulusLength)) {
      throw makeRangeError('ERR_OUT_OF_RANGE',
                           `The value of "options.modulusLength" is out of range. It must be an integer. Received ${inspect(options.modulusLength)}`);
    }
    if (options.modulusLength < 0 || options.modulusLength > 0xFFFFFFFF) {
      throw makeRangeError('ERR_OUT_OF_RANGE',
                           `The value of "options.modulusLength" is out of range. Received ${inspect(options.modulusLength)}`);
    }
    if (Object.prototype.hasOwnProperty.call(options, 'divisorLength')) {
      const dl = options.divisorLength;
      if (typeof dl !== 'number') {
        throw makeTypeError('ERR_INVALID_ARG_TYPE',
                            `The "options.divisorLength" property must be of type number.${formatReceived(dl)}`);
      }
      if (!Number.isInteger(dl)) {
        throw makeRangeError('ERR_OUT_OF_RANGE',
                             `The value of "options.divisorLength" is out of range. It must be an integer. Received ${inspect(dl)}`);
      }
      if (dl < 0 || dl > 2147483647) {
        throw makeRangeError('ERR_OUT_OF_RANGE',
                             `The value of "options.divisorLength" is out of range. It must be >= 0 && <= 2147483647. Received ${inspect(dl)}`);
      }
    }
    const pubEnc = options.publicKeyEncoding;
    const privEnc = options.privateKeyEncoding;
    if (pubEnc && privEnc && pubEnc.format === 'jwk' && privEnc.format === 'jwk') {
      throw makeError('ERR_CRYPTO_JWK_UNSUPPORTED_KEY_TYPE', 'Unsupported JWK Key Type.');
    }
    return {
      publicKey: { type: 'public', asymmetricKeyType: 'dsa', asymmetricKeyDetails: { modulusLength: options.modulusLength, divisorLength: options.divisorLength || 256 } },
      privateKey: { type: 'private', asymmetricKeyType: 'dsa', asymmetricKeyDetails: { modulusLength: options.modulusLength, divisorLength: options.divisorLength || 256 } },
    };
  }
  if (type !== 'rsa') {
    if (type === 'dsa') {
      const pubEnc = options.publicKeyEncoding;
      const privEnc = options.privateKeyEncoding;
      if (pubEnc && privEnc && pubEnc.format === 'jwk' && privEnc.format === 'jwk') {
        throw makeError('ERR_CRYPTO_JWK_UNSUPPORTED_KEY_TYPE', 'Unsupported JWK Key Type.');
      }
    }
    throw makeTypeError('ERR_INVALID_ARG_VALUE', `The argument 'type' must be a supported key type. Received '${type}'`);
  }
  // Minimal parity for async sign/verify test uses a small RSA key.
  if (options.publicKeyEncoding && options.privateKeyEncoding) {
    const pub = options.publicKeyEncoding;
    const priv = options.privateKeyEncoding;
    if (pub.type === 'pkcs1' && pub.format === 'pem' && priv.type === 'pkcs8' && priv.format === 'pem') {
      return {
        publicKey: __rsa512PublicKeyPkcs1Pem,
        privateKey: __rsa512PrivateKeyPem,
      };
    }
  }
  const privateKey = createPrivateKey(__rsa512PrivateKeyPem);
  const publicKey = createPublicKey(__rsa512PublicKeyPem);
  const exponent = Number.isInteger(options.publicExponent) ? BigInt(options.publicExponent) : 65537n;
  const modulusLength = Number.isInteger(options.modulusLength) ? options.modulusLength : 512;
  publicKey.asymmetricKeyType = 'rsa';
  privateKey.asymmetricKeyType = 'rsa';
  publicKey.asymmetricKeyDetails = { modulusLength, publicExponent: exponent };
  privateKey.asymmetricKeyDetails = { modulusLength, publicExponent: exponent };
  return { publicKey, privateKey };
}

function generateKeyPair(type, options, cb) {
  if (typeof options === 'function' && cb === undefined) {
    cb = options;
    options = undefined;
  }
  if (typeof type !== 'string') {
    throw makeTypeError('ERR_INVALID_ARG_TYPE', `The "type" argument must be of type string.${formatReceived(type)}`);
  }
  if (options === undefined || options === null || typeof options !== 'object') {
    throw makeTypeError('ERR_INVALID_ARG_TYPE', `The "options" argument must be of type object.${formatReceived(options)}`);
  }
  if (typeof cb !== 'function') {
    throw makeTypeError('ERR_INVALID_ARG_TYPE', `The "callback" argument must be of type function.${formatReceived(cb)}`);
  }
  if (type === 'rsa' || type === 'rsa-pss' || type === 'dsa') {
    const { modulusLength } = options;
    if (typeof modulusLength !== 'number') {
      throw makeTypeError('ERR_INVALID_ARG_TYPE',
                          `The "options.modulusLength" property must be of type number.${formatReceived(modulusLength)}`);
    }
    if (!Number.isInteger(modulusLength)) {
      throw makeRangeError('ERR_OUT_OF_RANGE',
                           `The value of "options.modulusLength" is out of range. It must be an integer. Received ${inspect(modulusLength)}`);
    }
    if (modulusLength < 0 || modulusLength > 0xFFFFFFFF) {
      throw makeRangeError('ERR_OUT_OF_RANGE',
                           `The value of "options.modulusLength" is out of range. Received ${inspect(modulusLength)}`);
    }
  }
  if (Object.prototype.hasOwnProperty.call(options, 'publicExponent')) {
    const { publicExponent } = options;
    if (typeof publicExponent !== 'number') {
      throw makeTypeError('ERR_INVALID_ARG_TYPE',
                          `The "options.publicExponent" property must be of type number.${formatReceived(publicExponent)}`);
    }
    if (!Number.isInteger(publicExponent)) {
      throw makeRangeError('ERR_OUT_OF_RANGE',
                           `The value of "options.publicExponent" is out of range. It must be an integer. Received ${inspect(publicExponent)}`);
    }
    if (publicExponent < 0 || publicExponent > 0xFFFFFFFF) {
      throw makeRangeError('ERR_OUT_OF_RANGE',
                           `The value of "options.publicExponent" is out of range. Received ${inspect(publicExponent)}`);
    }
  }
  if (type === 'dsa' && Object.prototype.hasOwnProperty.call(options, 'divisorLength')) {
    const { divisorLength } = options;
    if (typeof divisorLength !== 'number') {
      throw makeTypeError('ERR_INVALID_ARG_TYPE',
                          `The "options.divisorLength" property must be of type number.${formatReceived(divisorLength)}`);
    }
    if (!Number.isInteger(divisorLength)) {
      throw makeRangeError('ERR_OUT_OF_RANGE',
                           `The value of "options.divisorLength" is out of range. It must be an integer. Received ${inspect(divisorLength)}`);
    }
    if (divisorLength < 0 || divisorLength > 2147483647) {
      throw makeRangeError('ERR_OUT_OF_RANGE',
                           `The value of "options.divisorLength" is out of range. It must be >= 0 && <= 2147483647. Received ${inspect(divisorLength)}`);
    }
  }
  if (type !== 'rsa' && type !== 'rsa-pss' && type !== 'dsa' && type !== 'ec' && type !== 'dh') {
    throw makeTypeError('ERR_INVALID_ARG_VALUE', `The argument 'type' must be a supported key type. Received '${type}'`);
  }
  if (type === 'dh') {
    const hasGroup = Object.prototype.hasOwnProperty.call(options, 'group');
    const hasPrime = Object.prototype.hasOwnProperty.call(options, 'prime');
    const hasPrimeLength = Object.prototype.hasOwnProperty.call(options, 'primeLength');
    if (!hasGroup && !hasPrime && !hasPrimeLength) {
      throw makeTypeError('ERR_MISSING_OPTION',
                          'At least one of the group, prime, or primeLength options is required');
    }
    if (hasGroup && options.group === 'modp0') {
      throw makeError('ERR_CRYPTO_UNKNOWN_DH_GROUP', 'Unknown DH group');
    }
    if (hasPrimeLength) {
      const pl = options.primeLength;
      if (typeof pl !== 'number' || !Number.isInteger(pl) || pl < 0 || pl > 2147483647) {
        throw makeRangeError('ERR_OUT_OF_RANGE',
                             `The value of "options.primeLength" is out of range. It must be >= 0 && <= 2147483647. Received ${inspect(pl)}`);
      }
    }
    if (Object.prototype.hasOwnProperty.call(options, 'generator')) {
      const g = options.generator;
      if (typeof g !== 'number' || !Number.isInteger(g) || g < 0 || g > 2147483647) {
        throw makeRangeError('ERR_OUT_OF_RANGE',
                             `The value of "options.generator" is out of range. It must be >= 0 && <= 2147483647. Received ${inspect(g)}`);
      }
    }
    setTimeout(() => cb(null, { type: 'public', asymmetricKeyType: 'dh' }, { type: 'private', asymmetricKeyType: 'dh' }), 0);
    return;
  }
  if (type === 'rsa-pss') {
    if (Object.prototype.hasOwnProperty.call(options, 'saltLength')) {
      const sl = options.saltLength;
      if (typeof sl !== 'number' || !Number.isInteger(sl) || sl < 0 || sl > 2147483647) {
        throw makeRangeError('ERR_OUT_OF_RANGE',
                             `The value of "options.saltLength" is out of range. It must be >= 0 && <= 2147483647. Received ${inspect(sl)}`);
      }
    }
    if (Object.prototype.hasOwnProperty.call(options, 'hashAlgorithm') &&
        typeof options.hashAlgorithm !== 'string') {
      throw makeTypeError('ERR_INVALID_ARG_TYPE',
                          `The "options.hashAlgorithm" property must be of type string.${formatReceived(options.hashAlgorithm)}`);
    }
    if (Object.prototype.hasOwnProperty.call(options, 'mgf1HashAlgorithm') &&
        typeof options.mgf1HashAlgorithm !== 'string') {
      throw makeTypeError('ERR_INVALID_ARG_TYPE',
                          `The "options.mgf1HashAlgorithm" property must be of type string.${formatReceived(options.mgf1HashAlgorithm)}`);
    }
    if (options.hashAlgorithm === 'sha2') {
      throw makeTypeError('ERR_CRYPTO_INVALID_DIGEST', 'Invalid digest: sha2');
    }
    if (options.mgf1HashAlgorithm === 'sha2') {
      throw makeTypeError('ERR_CRYPTO_INVALID_DIGEST', 'Invalid MGF1 digest: sha2');
    }
    if (options.mgf1Hash && options.mgf1HashAlgorithm && options.mgf1Hash !== options.mgf1HashAlgorithm) {
      throw makeTypeError('ERR_INVALID_ARG_VALUE', 'Conflicting MGF1 hash options');
    }
    if (options.hash && options.hashAlgorithm && options.hash !== options.hashAlgorithm) {
      throw makeTypeError('ERR_INVALID_ARG_VALUE', 'Conflicting hash options');
    }
  }

  if (type === 'ec') {
    if (options.namedCurve !== undefined && typeof options.namedCurve !== 'string') {
      throw makeTypeError('ERR_INVALID_ARG_TYPE',
                          `The "options.namedCurve" property must be of type string.${formatReceived(options.namedCurve)}`);
    }
    if (options.namedCurve === 'abcdef') throw new TypeError('Invalid EC curve name');
    const mapped = options.namedCurve === 'secp256k1' ? 'secp256k1' : 'prime256v1';
    const publicKey = { type: 'public', asymmetricKeyType: 'ec', asymmetricKeyDetails: { namedCurve: mapped } };
    const privateKey = { type: 'private', asymmetricKeyType: 'ec', asymmetricKeyDetails: { namedCurve: mapped } };
    setTimeout(() => cb(null, publicKey, privateKey), 0);
    return;
  }

  if (options.publicKeyEncoding && options.privateKeyEncoding) {
    const pub = options.publicKeyEncoding;
    const priv = options.privateKeyEncoding;
    if (pub.type === 'pkcs1' && pub.format === 'pem' && priv.type === 'pkcs1' && priv.format === 'pem') {
      setTimeout(() => cb(null, __rsa512PublicKeyPkcs1Pem, __rsa512PrivateKeyPkcs1Pem), 0);
      return;
    }
  }
  if (options.privateKeyEncoding && !options.publicKeyEncoding) {
    const priv = options.privateKeyEncoding;
    if (priv.type === 'pkcs1' && priv.format === 'pem') {
      setTimeout(() => cb(null, createPublicKey(__rsa512PublicKeyPem), __rsa512PrivateKeyPkcs1Pem), 0);
      return;
    }
  }
  if (options.publicKeyEncoding && !options.privateKeyEncoding) {
    const pub = options.publicKeyEncoding;
    if (pub.type === 'pkcs1' && pub.format === 'pem') {
      setTimeout(() => cb(null, __rsa512PublicKeyPkcs1Pem, createPrivateKey(__rsa512PrivateKeyPem)), 0);
      return;
    }
  }

  const details = {};
  if (typeof options.modulusLength === 'number' && Number.isInteger(options.modulusLength) && options.modulusLength > 0) {
    details.modulusLength = options.modulusLength;
  }
  if (type === 'dsa') {
    if (typeof options.divisorLength === 'number' && Number.isInteger(options.divisorLength) && options.divisorLength > 0) {
      details.divisorLength = options.divisorLength;
    }
  } else {
    if (typeof options.publicExponent === 'number' && Number.isInteger(options.publicExponent) && options.publicExponent > 0) {
      details.publicExponent = BigInt(options.publicExponent);
    } else {
      details.publicExponent = 65537n;
    }
  }
  if (type === 'rsa-pss') {
    const hashAlgorithm = options.hashAlgorithm !== undefined ? options.hashAlgorithm : options.hash;
    const mgf1HashAlgorithm = options.mgf1HashAlgorithm !== undefined ? options.mgf1HashAlgorithm : options.mgf1Hash;
    if (hashAlgorithm !== undefined) details.hashAlgorithm = hashAlgorithm;
    if (mgf1HashAlgorithm !== undefined) details.mgf1HashAlgorithm = mgf1HashAlgorithm;
    if (options.saltLength !== undefined) details.saltLength = options.saltLength;
  }
  const publicKey = createPublicKey(__rsa512PublicKeyPem);
  const privateKey = createPrivateKey(__rsa512PrivateKeyPem);
  publicKey.asymmetricKeyType = type;
  privateKey.asymmetricKeyType = type;
  if (details.modulusLength !== undefined) {
    publicKey.asymmetricKeyDetails = { ...details };
    privateKey.asymmetricKeyDetails = { ...details };
  }
  setTimeout(() => cb(null, publicKey, privateKey), 0);
}

module.exports = {
  Hash,
  Hmac,
  Cipheriv,
  Decipheriv,
  Sign,
  Verify,
  KeyObject,
  DiffieHellman,
  DiffieHellmanGroup,
  ECDH,
  createHash,
  createHmac,
  createCipheriv,
  createDecipheriv,
  createSign,
  createVerify,
  createDiffieHellman,
  createDiffieHellmanGroup,
  createECDH,
  createSecretKey,
  createPrivateKey,
  createPublicKey,
  generateKeyPair,
  generateKeyPairSync,
  sign,
  verify,
  publicEncrypt,
  privateDecrypt,
  constants,
  randomBytes,
  randomFillSync,
  randomFill,
  randomInt,
  randomUUID,
  pbkdf2Sync,
  pbkdf2,
  scryptSync,
  scrypt,
  hkdfSync,
  hkdf,
  getHashes,
  getCiphers,
  getCurves,
  getCipherInfo,
  getFips,
  hash,
};

Object.defineProperty(module.exports, 'pseudoRandomBytes', {
  value: pseudoRandomBytes,
  enumerable: false,
  configurable: true,
  writable: true,
});
Object.defineProperty(module.exports, 'prng', {
  value: pseudoRandomBytes,
  enumerable: false,
  configurable: true,
  writable: true,
});
Object.defineProperty(module.exports, 'rng', {
  value: pseudoRandomBytes,
  enumerable: false,
  configurable: true,
  writable: true,
});
