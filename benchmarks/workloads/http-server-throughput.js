'use strict';

const http = require('node:http');
const body = Buffer.from('hello world\n');
const headers = {
  'content-type': 'text/plain',
  'content-length': String(body.length),
};
const server = http.createServer((request, response) => {
  response.writeHead(200, headers);
  response.end(body);
});
server.keepAliveTimeout = 60_000;
server.listen(Number(process.env.PORT || 3000), '127.0.0.1', () => {
  console.log('HTTP_BENCH_READY');
});
