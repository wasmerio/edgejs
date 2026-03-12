'use strict';

try {
  const dns = require('dns');
  console.log(typeof dns.lookup, typeof dns.resolve4);
} catch (err) {
  console.error(err && err.stack ? err.stack : err);
  process.exitCode = 1;
}
