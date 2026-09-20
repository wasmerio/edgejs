'use strict';

const assert = require('node:assert/strict');
const { createHash, hash: oneShotHash } = require('node:crypto');

const sha256abc = 'ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad';
for (const input of [Buffer.from('abc'),
                     new Uint8Array([0, 97, 98, 99, 0]).subarray(1, 4),
                     new DataView(new Uint8Array([0, 97, 98, 99, 0]).buffer, 1, 3)]) {
  assert.equal(createHash('sha256').update(input).digest('hex'), sha256abc);
  assert.equal(oneShotHash('sha256', input), sha256abc);
}
for (const [text, encoding] of [['abc', 'utf8'], ['616263', 'hex'], ['YWJj', 'base64']]) {
  assert.equal(createHash('sha256').update(text, encoding).digest('hex'), sha256abc);
}
const root = createHash('sha256').update('a');
const fork = root.copy();
assert.equal(root.update('bc').digest('hex'), sha256abc);
assert.equal(fork.update('bd').digest('hex'), createHash('sha256').update('abd').digest('hex'));
assert.throws(() => root.update('d'), { code: 'ERR_CRYPTO_HASH_FINALIZED' });
assert.throws(() => root.digest(), { code: 'ERR_CRYPTO_HASH_FINALIZED' });
assert.throws(() => root.copy(), { code: 'ERR_CRYPTO_HASH_FINALIZED' });
assert.throws(() => createHash('sha256', { outputLength: 31 }),
              { code: 'ERR_OSSL_EVP_NOT_XOF_OR_INVALID_LENGTH' });
assert.equal(createHash('sha256', { outputLength: 32 }).update('abc').digest('hex'), sha256abc);
for (const empty of [Buffer.alloc(0), '', new DataView(new ArrayBuffer(0))]) {
  assert.equal(createHash('sha256').update(empty).digest('hex'),
               'e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855');
  assert.equal(oneShotHash('sha256', empty),
               'e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855');
}

for (const [algorithm, defaultLength, expected] of [
  ['shake128', 16, '5881092dd818bf5cf8a3ddb793fbcba74097d5c526a6d35f97b83351940f2cc844c50af32acd3f2cdd066568706f509b'],
  ['shake256', 32, '483366601360a8771c6863080cc4114d8db44530f8f1e1ee4f94ea37e78b5739d5a15bef186a5386c75744c0527e1faa'],
]) {
  const hash = createHash(algorithm, { outputLength: 48 }).update('a');
  const extended = hash.copy({ outputLength: 48 });
  // Like Node, copy() without options uses the algorithm's default length.
  assert.equal(hash.copy().update('bc').digest().length, defaultLength);
  assert.equal(extended.update('bc').digest('hex'), expected);
  assert.equal(hash.update('bc').digest('hex'), expected);
  assert.equal(oneShotHash(algorithm, 'abc', { outputLength: 48 }), expected);
  assert.equal(oneShotHash(algorithm, 'abc', { outputLength: 0 }), '');
  assert.equal(createHash(algorithm, { outputLength: 0 }).update('abc').digest().length, 0);
}

// Keep both live copies and finalized wrappers reachable: native hashing must
// retain only digest state, even when JS has not collected these objects yet.
// Run this fixture with a bounded WASIX guest heap (e.g. 192 MiB). The previous
// implementation retained a full 64 MiB input per wrapper and exhausted it.
const chunk = Buffer.alloc(1024 * 1024, 0xa5);
const retained = [];
const expected = '232f01fd1785a591fdb704374b621371e8b897483af6e56d5095e8335711d92d948e0fa456352558260c55ca3409233b75ff6f9c94b19660e39efd1cc00edd10';
for (let run = 0; run < 8; run++) {
  const hash = createHash('sha512');
  for (let i = 0; i < 64; i++) hash.update(chunk);
  const copy = hash.copy();
  assert.equal(hash.digest('hex'), expected);
  assert.equal(copy.digest('hex'), expected);
  retained.push(hash, copy);
}
// A large single update must use bounded bridge leases as well.
const large = Buffer.alloc(24 * 1024 * 1024, 0xa5);
assert.equal(createHash('sha256').update(large).digest('hex'),
             '24a8b3ab7003be4d71ade03506845991c6143d7903d4d343f99cb07b8e287225');
assert.equal(oneShotHash('sha256', large),
             '24a8b3ab7003be4d71ade03506845991c6143d7903d4d343f99cb07b8e287225');
assert.equal(retained.length, 16);
console.log('CRYPTO_HASH_STREAMING_OK');
