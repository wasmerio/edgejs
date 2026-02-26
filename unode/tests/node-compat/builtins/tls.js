'use strict';

const { Buffer } = require('buffer');

const binding = globalThis.__unode_crypto;
if (!binding) throw new Error('tls builtin requires __unode_crypto binding');

function toBuffer(value) {
  if (Buffer.isBuffer(value)) return value;
  if (ArrayBuffer.isView(value)) return Buffer.from(value.buffer, value.byteOffset, value.byteLength);
  if (typeof value === 'string') return Buffer.from(value);
  throw new TypeError('Invalid TLS input');
}

function createContextObject() {
  const context = {
    setOptions() {
      if (this !== context) throw new TypeError('Illegal invocation');
    },
  };
  return context;
}

function createSecureContext(options = {}) {
  if (options && Object.prototype.hasOwnProperty.call(options, 'pfx')) {
    const pfx = toBuffer(options.pfx);
    const passphrase = options.passphrase == null ? '' : String(options.passphrase);
    binding.parsePfx(pfx, passphrase);
  }
  if (options && Object.prototype.hasOwnProperty.call(options, 'crl')) {
    binding.parseCrl(toBuffer(options.crl));
  }
  return { context: createContextObject() };
}

function getCiphers() {
  return ['aes256-sha', 'tls_aes_128_ccm_8_sha256'].slice().sort();
}

module.exports = {
  createSecureContext,
  getCiphers,
};
