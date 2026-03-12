'use strict';

const net = require('net');

const server = net.createServer((socket) => {
  socket.end('pong');
});

server.listen(0, '127.0.0.1', () => {
  const addr = server.address();
  const client = net.connect(addr.port, '127.0.0.1');
  const chunks = [];
  client.on('data', (c) => chunks.push(c));
  client.on('end', () => {
    process.stdout.write(Buffer.concat(chunks).toString('utf8') + '\n');
    server.close();
  });
  client.on('error', (e) => {
    process.stderr.write(String(e && e.stack ? e.stack : e) + '\n');
    server.close();
    process.exitCode = 1;
  });
});
