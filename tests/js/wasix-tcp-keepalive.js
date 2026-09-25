'use strict';

// Run on WASIX with the stock libc sysroot. Timing options are deliberately
// unsupported there; enabling/disabling the basic flag must still succeed.
const assert = require('node:assert/strict');
const net = require('node:net');
const { once } = require('node:events');
const { TCP, constants } = process.binding('tcp_wrap');

async function main() {
  const server = net.createServer({ keepAlive: true, keepAliveInitialDelay: 60000 }, (socket) => {
    assert.equal(socket._handle.setKeepAlive(true, 0), 0);
    socket.on('error', (error) => { throw error; });
    socket.pipe(socket);
  });
  server.listen(Number(process.env.PORT || 0), '127.0.0.1');
  await once(server, 'listening');
  try {
    for (const mode of ['before-connect', 'handle-before-connect', 'connecting', 'connect-options']) {
      const options = { host: '127.0.0.1', port: server.address().port };
      let socket;
      if (mode === 'before-connect' || mode === 'handle-before-connect') {
        socket = mode === 'handle-before-connect'
          ? new net.Socket({ handle: new TCP(constants.SOCKET), manualStart: true })
          : new net.Socket();
        assert.equal(socket.setKeepAlive(true, 60000), socket);
        socket.connect(options);
      } else if (mode === 'connecting') {
        socket = net.connect(options);
        assert.equal(socket.setKeepAlive(true, 60000), socket);
      } else {
        socket = net.connect({ ...options, keepAlive: true, keepAliveInitialDelay: 60000 });
      }
      await once(socket, 'connect');
      try {
        // Check the native result as well: the public API ignores error codes.
        for (const delay of [0, 60, 2147483647]) {
          assert.equal(socket._handle.setKeepAlive(true, delay), 0, mode);
          assert.equal(socket._handle.setKeepAlive(false, delay), 0, mode);
        }
        assert.equal(socket.setKeepAlive(false), socket);
        assert.equal(socket.setKeepAlive(true, 0), socket);
        const response = once(socket, 'data');
        socket.write(mode);
        assert.equal((await response)[0].toString(), mode);
      } finally {
        const closed = once(socket, 'close');
        socket.destroy();
        await closed;
      }
    }
  } finally {
    const closed = once(server, 'close');
    server.close();
    await closed;
  }
  console.log('WASIX_TCP_KEEPALIVE_OK');
}

main().catch((error) => {
  console.error(error);
  process.exitCode = 1;
});
