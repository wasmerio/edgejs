'use strict';

const http = require('http');

const server = http.createServer((req, res) => {
  res.writeHead(200, { 'content-type': 'text/plain' });
  res.end('ok');
});

server.listen(0, '127.0.0.1', () => {
  const { port } = server.address();
  http.get(`http://127.0.0.1:${port}/`, (res) => {
    const chunks = [];
    res.on('data', (c) => chunks.push(c));
    res.on('end', () => {
      process.stdout.write(Buffer.concat(chunks).toString('utf8') + '\n');
      server.close();
    });
  }).on('error', (err) => {
    process.stderr.write(String(err && err.stack ? err.stack : err) + '\n');
    server.close();
    process.exitCode = 1;
  });
});
