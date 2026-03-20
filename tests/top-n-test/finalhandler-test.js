'use strict';

const assert = require('node:assert/strict');
const http = require('node:http');

try {
  const finalhandler = require('finalhandler');

  // finalhandler is a function
  assert.equal(typeof finalhandler, 'function');

  // Create a real HTTP server to test with
  const server = http.createServer(function (req, res) {
    const done = finalhandler(req, res);
    // Simulate a 404 by calling the handler with no error
    done();
  });

  server.listen(0, function () {
    const port = server.address().port;

    // Make a request to a non-existent path; finalhandler should respond with 404
    http.get('http://127.0.0.1:' + port + '/nonexistent', function (res) {
      let body = '';
      res.on('data', function (chunk) { body += chunk; });
      res.on('end', function () {
        assert.equal(res.statusCode, 404);

        // Now test with an error passed to the handler
        const server2 = http.createServer(function (req, res) {
          const done = finalhandler(req, res);
          const err = new Error('Something broke');
          err.status = 503;
          done(err);
        });

        server2.listen(0, function () {
          const port2 = server2.address().port;

          http.get('http://127.0.0.1:' + port2 + '/fail', function (res2) {
            let body2 = '';
            res2.on('data', function (chunk) { body2 += chunk; });
            res2.on('end', function () {
              assert.equal(res2.statusCode, 503);

              server.close();
              server2.close();
              console.log('finalhandler-test:ok');
            });
          });
        });
      });
    });
  });
} catch (err) {
  console.error('finalhandler-test:fail');
  console.error(err && err.stack ? err.stack : String(err));
  process.exit(1);
}
