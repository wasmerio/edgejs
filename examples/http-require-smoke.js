'use strict';

try {
  const http = require('http');
  console.log(typeof http.createServer, typeof http.request, typeof http.METHODS);
} catch (err) {
  console.error(err && err.stack ? err.stack : err);
  process.exitCode = 1;
}
