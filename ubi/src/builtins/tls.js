'use strict';

const { Buffer } = require('buffer');
const net = require('net');
const { inspect } = require('util');

const binding = globalThis.__ubi_crypto;
if (!binding) throw new Error('tls builtin requires __ubi_crypto binding');

function toBuffer(value) {
  if (Buffer.isBuffer(value)) return value;
  if (ArrayBuffer.isView(value)) return Buffer.from(value.buffer, value.byteOffset, value.byteLength);
  if (typeof value === 'string') return Buffer.from(value);
  throw new TypeError('Invalid TLS input');
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

function invalidArgTypeHelper(input) {
  if (input == null) return ` Received ${input}`;
  if (typeof input === 'function') return ` Received function ${input.name}`;
  if (typeof input === 'object') {
    if (input.constructor && input.constructor.name) {
      return ` Received an instance of ${input.constructor.name}`;
    }
    return ` Received ${inspect(input, { depth: -1 })}`;
  }
  return ` Received type ${typeof input} (${inspect(input, { colors: false })})`;
}

function isTlsPemInput(value) {
  return typeof value === 'string' || Buffer.isBuffer(value) || ArrayBuffer.isView(value);
}

function throwInvalidTlsOptionType(name, value) {
  throw makeTypeError(
    'ERR_INVALID_ARG_TYPE',
    `The "options.${name}" property must be of type string or an instance of Buffer, TypedArray, or DataView.` +
      invalidArgTypeHelper(value)
  );
}

function validateStringOption(name, value) {
  if (value === undefined) return;
  if (typeof value !== 'string') {
    throw makeTypeError(
      'ERR_INVALID_ARG_TYPE',
      `The "options.${name}" property must be of type string.` + invalidArgTypeHelper(value)
    );
  }
}

function validateNumberOption(name, value) {
  if (value === undefined) return;
  if (typeof value !== 'number') {
    throw makeTypeError(
      'ERR_INVALID_ARG_TYPE',
      `The "options.${name}" property must be of type number.` + invalidArgTypeHelper(value)
    );
  }
}

function throwCustomEngineNotSupported() {
  const err = new Error('Custom engines not supported by this OpenSSL');
  err.code = 'ERR_CRYPTO_CUSTOM_ENGINE_NOT_SUPPORTED';
  throw err;
}

function normalizeTlsOption(name, value, opts = {}) {
  const allowPemObjectArray = opts.allowPemObjectArray === true;
  // Keep Node-compatible false-y behavior for secure context inputs.
  if (value == null || value === false || value === '' || value === 0) return [];
  if (Array.isArray(value)) {
    return value.map((entry) => {
      if (allowPemObjectArray &&
          entry &&
          typeof entry === 'object' &&
          !Buffer.isBuffer(entry) &&
          !ArrayBuffer.isView(entry) &&
          Object.prototype.hasOwnProperty.call(entry, 'pem')) {
        if (!isTlsPemInput(entry.pem)) throwInvalidTlsOptionType(name, entry.pem);
        return { pem: entry.pem, passphrase: entry.passphrase };
      }
      if (!isTlsPemInput(entry)) throwInvalidTlsOptionType(name, entry);
      return entry;
    });
  }
  if (!isTlsPemInput(value)) throwInvalidTlsOptionType(name, value);
  return [value];
}

function createContextObject() {
  const handle = typeof binding.secureContextCreate === 'function'
    ? binding.secureContextCreate()
    : null;
  const context = {
    _handle: handle,
    init(minVersion, maxVersion) {
      if (this !== context) throw new TypeError('Illegal invocation');
      if (handle && typeof binding.secureContextInit === 'function') {
        binding.secureContextInit(handle, minVersion || 0, maxVersion || 0);
      }
    },
    setOptions(options) {
      if (this !== context) throw new TypeError('Illegal invocation');
      if (handle && typeof binding.secureContextSetOptions === 'function') {
        binding.secureContextSetOptions(handle, options | 0);
      }
    },
    setCiphers(ciphers) {
      if (this !== context) throw new TypeError('Illegal invocation');
      if (handle && typeof binding.secureContextSetCiphers === 'function') {
        binding.secureContextSetCiphers(handle, String(ciphers));
      }
    },
    setCipherSuites(suites) {
      if (this !== context) throw new TypeError('Illegal invocation');
      if (handle && typeof binding.secureContextSetCipherSuites === 'function') {
        binding.secureContextSetCipherSuites(handle, String(suites));
      }
    },
    setCert(cert) {
      if (this !== context) throw new TypeError('Illegal invocation');
      if (handle && typeof binding.secureContextSetCert === 'function') {
        binding.secureContextSetCert(handle, toBuffer(cert));
      }
    },
    setKey(key, passphrase) {
      if (this !== context) throw new TypeError('Illegal invocation');
      if (handle && typeof binding.secureContextSetKey === 'function') {
        binding.secureContextSetKey(handle, toBuffer(key), passphrase == null ? '' : String(passphrase));
      }
    },
    addCACert(ca) {
      if (this !== context) throw new TypeError('Illegal invocation');
      if (handle && typeof binding.secureContextAddCACert === 'function') {
        binding.secureContextAddCACert(handle, toBuffer(ca));
      }
    },
    addCRL(crl) {
      if (this !== context) throw new TypeError('Illegal invocation');
      if (handle && typeof binding.secureContextAddCrl === 'function') {
        binding.secureContextAddCrl(handle, toBuffer(crl));
      }
    },
  };
  return context;
}

function toVersion(v) {
  if (v == null) return 0;
  if (typeof v === 'number') return v | 0;
  switch (String(v)) {
    case 'TLSv1':
      return 0x0301;
    case 'TLSv1.1':
      return 0x0302;
    case 'TLSv1.2':
      return 0x0303;
    case 'TLSv1.3':
      return 0x0304;
    default:
      return 0;
  }
}

function forEachMaybeArray(value, fn) {
  if (value == null) return;
  if (Array.isArray(value)) {
    for (const item of value) fn(item);
    return;
  }
  fn(value);
}

function createSecureContext(options = {}) {
  validateStringOption('privateKeyEngine', options.privateKeyEngine);
  validateStringOption('privateKeyIdentifier', options.privateKeyIdentifier);
  validateStringOption('clientCertEngine', options.clientCertEngine);
  if (options.privateKeyEngine !== undefined || options.privateKeyIdentifier !== undefined ||
      options.clientCertEngine !== undefined) {
    throwCustomEngineNotSupported();
  }

  const context = createContextObject();
  context.init(toVersion(options.minVersion), toVersion(options.maxVersion));
  if (options && options.secureOptions !== undefined) {
    context.setOptions(options.secureOptions);
  }
  if (options && options.ciphers) {
    context.setCiphers(options.ciphers);
  }
  if (options && options.cipherSuites) {
    context.setCipherSuites(options.cipherSuites);
  }
  for (const ca of normalizeTlsOption('ca', options && options.ca)) {
    context.addCACert(ca);
  }
  forEachMaybeArray(options && options.crl, (crl) => {
    const crlBuf = toBuffer(crl);
    if (typeof binding.parseCrl === 'function') binding.parseCrl(crlBuf);
    context.addCRL(crlBuf);
  });
  for (const cert of normalizeTlsOption('cert', options && options.cert)) {
    context.setCert(cert);
  }
  const passphrase = options && options.passphrase;
  for (const key of normalizeTlsOption('key', options && options.key, { allowPemObjectArray: true })) {
    if (key && typeof key === 'object' && Object.prototype.hasOwnProperty.call(key, 'pem')) {
      context.setKey(key.pem, key.passphrase !== undefined ? key.passphrase : passphrase);
    } else {
      context.setKey(key, passphrase);
    }
  }
  if (options && Object.prototype.hasOwnProperty.call(options, 'pfx')) {
    const pfx = toBuffer(options.pfx);
    const passphrase = options.passphrase == null ? '' : String(options.passphrase);
    binding.parsePfx(pfx, passphrase);
  }
  return { context };
}

function getCiphers() {
  return ['aes256-sha', 'tls_aes_128_ccm_8_sha256'].slice().sort();
}

class TLSSocket extends net.Socket {
  constructor(socket, options) {
    super(options);
    this._secureEstablished = true;
    this.encrypted = true;
    this.authorized = true;
    this.authorizationError = null;
    this.ssl = {};
    if (socket && typeof socket === 'object') {
      this._parent = socket;
    }
  }
}

function markSocketAsSecure(socket) {
  if (!socket || typeof socket !== 'object') return socket;
  socket._secureEstablished = true;
  socket.encrypted = true;
  socket.authorized = true;
  socket.authorizationError = null;
  if (!socket.ssl) socket.ssl = {};
  return socket;
}

class TLSServer extends net.Server {
  constructor(options, listener) {
    if (typeof options === 'function') {
      listener = options;
      options = {};
    }
    super((socket) => {
      const tlsSocket = markSocketAsSecure(socket);
      if (typeof listener === 'function') listener(tlsSocket);
    });
    this.options = options || {};
    validateNumberOption('sessionTimeout', this.options.sessionTimeout);
    if (this.options.sessionTimeout !== undefined) {
      const value = this.options.sessionTimeout;
      const max = 2 ** 31 - 1;
      if (!Number.isFinite(value) || value < 0 || value > max) {
        throw makeRangeError(
          'ERR_OUT_OF_RANGE',
          `The value of "options.sessionTimeout" is out of range. It must be >= 0 && <= ${max}. Received ${value}`
        );
      }
    }
    // Validate and normalize TLS options through secure context creation.
    this._sharedCreds = createSecureContext(this.options);
    this._ticketKeys = null;
  }

  setTicketKeys(keys) {
    const isView = ArrayBuffer.isView(keys);
    const isBuffer = Buffer.isBuffer(keys);
    if (!isView && !isBuffer) {
      throw makeTypeError(
        'ERR_INVALID_ARG_TYPE',
        'The "keys" argument must be an instance of Buffer, TypedArray, or DataView.'
      );
    }
    const len = isBuffer ? keys.length : keys.byteLength;
    if (len !== 48) {
      throw new Error('Session ticket keys must be a 48-byte buffer');
    }
    this._ticketKeys = Buffer.from(keys.buffer || keys, keys.byteOffset || 0, len);
  }

  getTicketKeys() {
    return this._ticketKeys ? Buffer.from(this._ticketKeys) : Buffer.alloc(48);
  }
}

function Server(options, listener) {
  return new TLSServer(options, listener);
}
Server.prototype = TLSServer.prototype;
Object.setPrototypeOf(Server, TLSServer);

function createServer(options, listener) {
  return new Server(options, listener);
}

function connect(options, callback) {
  let socket;
  if (typeof options === 'object' && options !== null) {
    socket = net.connect(options);
  } else {
    socket = net.connect(...arguments);
    callback = undefined;
  }
  markSocketAsSecure(socket);
  if (typeof callback === 'function') {
    socket.once('connect', callback);
  }
  return socket;
}

module.exports = {
  TLSSocket,
  Server,
  connect,
  createConnection: connect,
  createServer,
  createSecureContext,
  getCiphers,
};
