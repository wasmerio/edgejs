'use strict';

const assert = require('node:assert/strict');
const http = require('node:http');

const express = require('express');

(async () => {
  // Create an express app
  const app = express();
  assert.equal(typeof app, 'function', 'express() should return a function');
  assert.equal(typeof app.get, 'function', 'app should have .get()');
  assert.equal(typeof app.post, 'function', 'app should have .post()');
  assert.equal(typeof app.use, 'function', 'app should have .use()');
  assert.equal(typeof app.listen, 'function', 'app should have .listen()');

  // Register middleware
  let middlewareCalled = false;
  app.use((req, res, next) => {
    middlewareCalled = true;
    next();
  });

  // Register routes
  app.get('/hello', (req, res) => {
    res.setHeader('Content-Type', 'text/plain');
    res.end('Hello World');
  });

  app.post('/echo', (req, res) => {
    res.json({ method: 'POST', path: '/echo' });
  });

  // Create Router
  const router = express.Router();
  assert.equal(typeof router, 'function', 'Router should be a function');
  assert.equal(typeof router.get, 'function', 'router should have .get()');

  router.get('/sub', (req, res) => {
    res.end('sub-route');
  });
  app.use('/api', router);

  // Start server on a random port and make a real request
  const server = app.listen(0);
  const port = server.address().port;

  // Helper to make a simple HTTP request
  function request(method, path) {
    return new Promise((resolve, reject) => {
      const req = http.request({ hostname: '127.0.0.1', port, path, method }, (res) => {
        let body = '';
        res.on('data', (chunk) => { body += chunk; });
        res.on('end', () => resolve({ status: res.statusCode, body, headers: res.headers }));
      });
      req.on('error', reject);
      req.end();
    });
  }

  try {
    // Test GET /hello
    const resp = await request('GET', '/hello');
    assert.equal(resp.status, 200, 'GET /hello should return 200');
    assert.equal(resp.body, 'Hello World', 'GET /hello body should be correct');
    assert.ok(middlewareCalled, 'middleware should have been called');

    // Test GET /api/sub via router
    const subResp = await request('GET', '/api/sub');
    assert.equal(subResp.status, 200, 'GET /api/sub should return 200');
    assert.equal(subResp.body, 'sub-route');

    // Test POST /echo
    const postResp = await request('POST', '/echo');
    assert.equal(postResp.status, 200);
    const parsed = JSON.parse(postResp.body);
    assert.equal(parsed.method, 'POST');
    assert.equal(parsed.path, '/echo');

    // Test 404 for unknown route
    const notFound = await request('GET', '/nonexistent');
    assert.equal(notFound.status, 404, 'unknown route should return 404');

    // Check static middleware exists
    assert.equal(typeof express.static, 'function', 'express.static should exist');
    assert.equal(typeof express.json, 'function', 'express.json should exist');
    assert.equal(typeof express.urlencoded, 'function', 'express.urlencoded should exist');
  } finally {
    server.close();
  }

  console.log('express-test:ok');
})().catch((err) => {
  console.error('express-test:fail');
  console.error(err && err.stack ? err.stack : String(err));
  process.exit(1);
});
