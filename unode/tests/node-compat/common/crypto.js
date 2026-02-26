'use strict';

const assert = require('assert');
const crypto = require('crypto');

function getRegExpForPEM(label, cipher) {
  const head = `\\-\\-\\-\\-\\-BEGIN ${label}\\-\\-\\-\\-\\-`;
  const rfc1421Header = cipher == null ?
    '' :
    `\nProc-Type: 4,ENCRYPTED\nDEK-Info: ${cipher},[^\n]+\n`;
  const body = '([a-zA-Z0-9\\+/=]{64}\n)*[a-zA-Z0-9\\+/=]{1,64}';
  const end = `\\-\\-\\-\\-\\-END ${label}\\-\\-\\-\\-\\-`;
  return new RegExp(`^${head}${rfc1421Header}\n${body}\n${end}\n$`);
}

function assertApproximateSize(key, expectedSize) {
  const min = Math.floor(0.9 * expectedSize);
  const max = Math.ceil(1.1 * expectedSize);
  assert(key.length >= min, `Key (${key.length}) is shorter than expected (${min})`);
  assert(key.length <= max, `Key (${key.length}) is longer than expected (${max})`);
}

function testEncryptDecrypt(publicKey, privateKey) {
  if ((publicKey && publicKey.asymmetricKeyType === 'rsa-pss') ||
      (privateKey && privateKey.asymmetricKeyType === 'rsa-pss')) {
    throw new Error('operation not supported for this keytype');
  }
  if (typeof crypto.publicEncrypt !== 'function' || typeof crypto.privateDecrypt !== 'function') {
    return;
  }
  const message = Buffer.from('Hello Node.js world!');
  for (const key of [publicKey, privateKey]) {
    const ciphertext = crypto.publicEncrypt(key, message);
    const received = crypto.privateDecrypt(privateKey, ciphertext);
    assert.strictEqual(received.toString('utf8'), 'Hello Node.js world!');
  }
}

function testSignVerify(publicKey, privateKey) {
  if ((publicKey && publicKey.padding === crypto.constants.RSA_PKCS1_PADDING) ||
      (privateKey && privateKey.padding === crypto.constants.RSA_PKCS1_PADDING)) {
    throw new Error('illegal or unsupported padding mode');
  }
  const message = Buffer.from('Hello Node.js world!');
  const signature = crypto.sign('SHA256', message, privateKey);
  assert(crypto.verify('SHA256', message, publicKey, signature));
  assert(crypto.verify('SHA256', message, privateKey, signature));
  const legacySig = crypto.createSign('SHA256').update(message).sign(privateKey);
  assert(crypto.createVerify('SHA256').update(message).verify(publicKey, legacySig));
}

function hasOpenSSL(major = 0, minor = 0, patch = 0) {
  const v = process && process.versions && process.versions.openssl;
  if (!v) return false;
  const m = /(\d+)\.(\d+)\.(\d+)/.exec(String(v));
  if (!m) return false;
  const cur = [Number(m[1]), Number(m[2]), Number(m[3])];
  const req = [Number(major), Number(minor), Number(patch)];
  if (cur[0] !== req[0]) return cur[0] > req[0];
  if (cur[1] !== req[1]) return cur[1] > req[1];
  return cur[2] >= req[2];
}

module.exports = {
  assertApproximateSize,
  testEncryptDecrypt,
  testSignVerify,
  pkcs1PubExp: getRegExpForPEM('RSA PUBLIC KEY'),
  pkcs1PrivExp: getRegExpForPEM('RSA PRIVATE KEY'),
  pkcs1EncExp: (cipher) => getRegExpForPEM('RSA PRIVATE KEY', cipher),
  spkiExp: getRegExpForPEM('PUBLIC KEY'),
  pkcs8Exp: getRegExpForPEM('PRIVATE KEY'),
  pkcs8EncExp: getRegExpForPEM('ENCRYPTED PRIVATE KEY'),
  sec1Exp: getRegExpForPEM('EC PRIVATE KEY'),
  sec1EncExp: (cipher) => getRegExpForPEM('EC PRIVATE KEY', cipher),
  hasOpenSSL,
  get hasOpenSSL3() {
    return hasOpenSSL(3);
  },
};
