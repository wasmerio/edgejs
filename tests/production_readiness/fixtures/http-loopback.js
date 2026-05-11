'use strict';

const http = require('node:http');

const server = http.createServer((req, res) => {
  if (req.url !== '/ready') {
    res.statusCode = 404;
    res.end('not-found');
    return;
  }
  res.end('ok');
});

server.listen(0, '127.0.0.1', () => {
  const { port } = server.address();
  http.get({ hostname: '127.0.0.1', port, path: '/ready' }, (res) => {
    let body = '';
    res.setEncoding('utf8');
    res.on('data', (chunk) => {
      body += chunk;
    });
    res.on('end', () => {
      server.close(() => {
        if (res.statusCode !== 200 || body !== 'ok') {
          console.error(`unexpected http response: ${res.statusCode} ${body}`);
          process.exit(1);
        }
        console.log('production-readiness:http-loopback');
      });
    });
  }).on('error', (err) => {
    server.close(() => {
      console.error(err && (err.stack || err.message || err));
      process.exit(1);
    });
  });
});
