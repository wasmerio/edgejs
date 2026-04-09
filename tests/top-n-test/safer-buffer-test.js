'use strict';

const assert = require('node:assert/strict');

try {
  const { Buffer: SafeBuffer } = require('safer-buffer');

  // Buffer.from with a string
  const buf1 = SafeBuffer.from('hello world', 'utf8');
  assert.equal(buf1.toString('utf8'), 'hello world');

  // Buffer.from with an array
  const buf2 = SafeBuffer.from([72, 101, 108, 108, 111]);
  assert.equal(buf2.toString('utf8'), 'Hello');

  // Buffer.alloc creates a zero-filled buffer
  const buf3 = SafeBuffer.alloc(16);
  assert.equal(buf3.length, 16);
  for (let i = 0; i < buf3.length; i++) {
    assert.equal(buf3[i], 0);
  }

  // Buffer.alloc with fill value
  const buf4 = SafeBuffer.alloc(8, 0xff);
  assert.equal(buf4.length, 8);
  for (let i = 0; i < buf4.length; i++) {
    assert.equal(buf4[i], 0xff);
  }

  // safer-buffer intentionally removes allocUnsafe - verify that
  assert.equal(SafeBuffer.allocUnsafe, undefined, 'allocUnsafe should not exist on safe Buffer');
  assert.equal(SafeBuffer.allocUnsafeSlow, undefined, 'allocUnsafeSlow should not exist on safe Buffer');

  // Convert between encodings
  const original = 'Encoding test: ABC 123';
  const asBase64 = SafeBuffer.from(original, 'utf8').toString('base64');
  const backToUtf8 = SafeBuffer.from(asBase64, 'base64').toString('utf8');
  assert.equal(backToUtf8, original);

  // Hex encoding round-trip
  const asHex = SafeBuffer.from(original, 'utf8').toString('hex');
  const fromHex = SafeBuffer.from(asHex, 'hex').toString('utf8');
  assert.equal(fromHex, original);

  // Compare with Node's native Buffer behavior
  const nativeBuf = Buffer.from('test');
  const safeBuf = SafeBuffer.from('test');
  assert.equal(nativeBuf.toString(), safeBuf.toString());
  assert.equal(nativeBuf.length, safeBuf.length);

  // Buffer.from another buffer (copy)
  const source = SafeBuffer.from('copy me');
  const copy = SafeBuffer.from(source);
  assert.equal(copy.toString(), 'copy me');
  // Verify it's a true copy, not a reference
  source[0] = 0x58; // 'X'
  assert.notEqual(copy[0], 0x58);

  console.log('safer-buffer-test:ok');
} catch (err) {
  console.error('safer-buffer-test:fail');
  console.error(err && err.stack ? err.stack : String(err));
  process.exit(1);
}
