'use strict';
// Regression test: when a worker's env is torn down, every N-API finalizer
// runs in one pass, in an order that refcounts no longer constrain. Live
// Http2Streams are still registered in their Http2Session's stream map, and
// the session is still listening on its parent JSStream. Neither finalizer
// may touch the other (already freed) object.
const assert = require('assert');
const http2 = require('http2');
const { duplexPair } = require('stream');
const { Worker, isMainThread, parentPort, workerData } = require('worker_threads');

const kStreams = 8;

if (isMainThread) {
  const modes = ['exit', 'terminate'];
  const rounds = 3;
  let exits = 0;
  for (let i = 0; i < rounds; i++) {
    for (const mode of modes) {
      const w = new Worker(__filename, { workerData: { mode } });
      w.on('message', () => w.terminate());
      w.on('error', (err) => { throw err; });
      w.on('exit', () => { exits++; });
    }
  }
  process.on('exit', () => assert.strictEqual(exits, rounds * modes.length));
} else {
  const server = http2.createServer();
  const [clientSide, serverSide] = duplexPair();
  server.emit('connection', serverSide);
  const client = http2.connect('http://localhost:80', {
    createConnection: () => clientSide,
  });

  // Requests are never answered, so client and server streams all stay in
  // their sessions' stream maps until the worker is torn down.
  const keep = [];
  for (let i = 0; i < kStreams; i++) keep.push(client.request());
  let seen = 0;
  server.on('stream', (stream) => {
    keep.push(stream);
    if (++seen !== kStreams) return;
    if (workerData.mode === 'exit') process.exit(0);
    parentPort.postMessage('terminate');
  });
}
