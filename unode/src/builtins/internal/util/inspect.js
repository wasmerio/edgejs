'use strict';

const util = require('util');

function stripVTControlCharacters(str) {
  return String(str).replace(/\u001b\[[0-9;]*[A-Za-z]/g, '');
}

function getStringWidth(str) {
  let width = 0;
  for (const ch of String(str)) {
    const code = ch.codePointAt(0);
    if (code <= 0x1f || (code >= 0x7f && code <= 0x9f)) continue;
    width += code > 0xff ? 2 : 1;
  }
  return width;
}

function identicalSequenceRange(a, b) {
  for (let i = 0; i < a.length - 3; i++) {
    const pos = b.indexOf(a[i]);
    if (pos !== -1) {
      const rest = b.length - pos;
      if (rest > 3) {
        let len = 1;
        const maxLen = Math.min(a.length - i, rest);
        while (maxLen > len && a[i + len] === b[pos + len]) {
          len++;
        }
        if (len > 3) {
          return [len, i];
        }
      }
    }
  }
  return [0, 0];
}

module.exports = {
  inspect: util.inspect || ((value) => String(value)),
  identicalSequenceRange,
  getStringWidth,
  stripVTControlCharacters,
};
