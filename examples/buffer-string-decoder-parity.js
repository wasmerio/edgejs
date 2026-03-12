'use strict';

const { StringDecoder } = require('string_decoder');

function showCase(title, bytes, encoding) {
  const buf = Buffer.from(bytes);
  const decoder = new StringDecoder(encoding);

  const left = Math.floor(buf.length / 2);
  const first = buf.subarray(0, left);
  const second = buf.subarray(left);

  const decodedByBuffer = buf.toString(encoding);
  const decodedByDecoder = decoder.write(first) + decoder.write(second) + decoder.end();

  console.log(`\n${title}`);
  console.log(`encoding: ${encoding}`);
  console.log(`bytes: ${buf.toString('hex')}`);
  console.log(`Buffer.toString(): ${JSON.stringify(decodedByBuffer)}`);
  console.log(`StringDecoder:    ${JSON.stringify(decodedByDecoder)}`);
  console.log(`equal: ${decodedByBuffer === decodedByDecoder}`);
}

showCase(
  'Invalid UTF-8 replacement behavior',
  [0xC9, 0xB5, 0xA9, 0x41],
  'utf8',
);

showCase(
  'Latin1 byte-preserving behavior',
  [0xB5, 0x61, 0x62, 0xE8, 0x63],
  'latin1',
);

showCase(
  'UTF-16LE split character behavior',
  [0x3D, 0xD8, 0x4D, 0xDC, 0x61, 0x00],
  'utf16le',
);
