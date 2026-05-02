'use strict';

const assert = require('node:assert/strict');

(async () => {
  const WebSocket = require('ws');

  // Create a WebSocket server on a random port
  const wss = new WebSocket.Server({ port: 0 });

  try {
    // Server should be listening
    const addr = wss.address();
    assert.ok(addr, 'server should have an address');
    assert.equal(typeof addr.port, 'number');
    assert.ok(addr.port > 0, 'port should be positive');

    // Exchange a message between client and server
    const messageReceived = new Promise((resolve, reject) => {
      const timeout = setTimeout(() => reject(new Error('timeout')), 5000);

      wss.on('connection', (serverSocket) => {
        // Server echoes messages back with a prefix
        serverSocket.on('message', (data) => {
          serverSocket.send('echo:' + data.toString());
        });
      });

      const client = new WebSocket(`ws://127.0.0.1:${addr.port}`);

      client.on('open', () => {
        client.send('hello');
      });

      client.on('message', (data) => {
        clearTimeout(timeout);
        client.close();
        resolve(data.toString());
      });

      client.on('error', (err) => {
        clearTimeout(timeout);
        reject(err);
      });
    });

    const echoResponse = await messageReceived;
    assert.equal(echoResponse, 'echo:hello');

    // Multiple clients can connect
    const client2Response = new Promise((resolve, reject) => {
      const timeout = setTimeout(() => reject(new Error('timeout')), 5000);
      const client2 = new WebSocket(`ws://127.0.0.1:${addr.port}`);

      client2.on('open', () => {
        client2.send('world');
      });

      client2.on('message', (data) => {
        clearTimeout(timeout);
        client2.close();
        resolve(data.toString());
      });

      client2.on('error', (err) => {
        clearTimeout(timeout);
        reject(err);
      });
    });

    assert.equal(await client2Response, 'echo:world');

    // WebSocket constants are available
    assert.equal(WebSocket.OPEN, 1);
    assert.equal(WebSocket.CLOSED, 3);
    assert.equal(WebSocket.CONNECTING, 0);
    assert.equal(WebSocket.CLOSING, 2);

  } finally {
    // Close the server
    await new Promise((resolve) => wss.close(resolve));
  }

  console.log('ws-test:ok');
})().catch((err) => {
  console.error('ws-test:fail');
  console.error(err && err.stack ? err.stack : String(err));
  process.exit(1);
});
