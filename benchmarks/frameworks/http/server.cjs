const http = require('node:http');

http.createServer((req, res) => {
  if (req.method !== 'GET' || req.url !== '/api/bench') {
    res.writeHead(404).end();
    return;
  }
  res.writeHead(200, {
    'Content-Type': 'text/plain; charset=utf-8',
    'Content-Length': '11',
  });
  res.end('hello edge\n');
}).listen(Number(process.env.PORT || 3000), process.env.HOST || '127.0.0.1');
