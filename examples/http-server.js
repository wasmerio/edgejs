// server.js
const http = require('http');

const PORT = process.env.PORT || 4200;

const server = http.createServer((req, res) => {
  // Basic routing
  if (req.method === 'GET' && req.url === '/') {
    res.writeHead(200, { 'Content-Type': 'application/json' });
    res.end(JSON.stringify({ message: 'Hello, World!' }));
    return;
  }

  if (req.method === 'GET' && req.url === '/health') {
    res.writeHead(200, { 'Content-Type': 'text/plain' });
    res.end('OK');
    return;
  }

  // 404 fallback
  res.writeHead(404, { 'Content-Type': 'application/json' });
  res.end(JSON.stringify({ error: 'Not Found' }));
});

server.on('error', (err) => {
  const code = err && err.code ? err.code : 'UNKNOWN';
  const message = err && err.message ? err.message : String(err);
  console.error(`Failed to start server on port ${PORT}: [${code}] ${message}`);
  process.exitCode = 1;
});

server.listen(PORT, () => {
  console.log(`Server running on http://localhost:${PORT}`);
});