const SD = require('string_decoder').StringDecoder;

const encodings = ['base64', 'base64url', 'hex', 'utf8', 'utf16le', 'ucs2'];
const bufs = [ '☃💩', 'asdf' ].map((b) => Buffer.from(b));
for (let i = 1; i <= 16; i++) {
  const bytes = '.'.repeat(i - 1).split('.').map((_, j) => j + 0x78);
  bufs.push(Buffer.from(bytes));
}

for (const encoding of encodings) {
  for (const buf of bufs) {
    let s = new SD(encoding);
    let res1 = '';
    for (let i = 0; i < buf.length; i++) res1 += s.write(buf.slice(i, i + 1));
    res1 += s.end();

    s = new SD(encoding);
    let res2 = '';
    res2 += s.write(buf);
    res2 += s.end();

    const res3 = buf.toString(encoding);
    if (res1 !== res3 || res2 !== res3) {
      console.log('mismatch encoding=', encoding, 'hex=', buf.toString('hex'));
      console.log('res1=', JSON.stringify(res1));
      console.log('res2=', JSON.stringify(res2));
      console.log('res3=', JSON.stringify(res3));
      process.exit(1);
    }
  }
}
console.log('ok');
