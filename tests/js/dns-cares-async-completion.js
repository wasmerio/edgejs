'use strict';

// c-ares may complete a query from inside the call that issued it (an
// immediate send failure, an /etc/hosts hit, or ares_cancel()). Like Node,
// the binding must still report the result on a later loop turn. Otherwise a
// dns.promises query rejects inside its Promise executor, before the caller
// attaches a handler, and the process dies with an unhandled rejection.
const assert = require('node:assert/strict');
const dns = require('node:dns');

process.on('unhandledRejection', (error) => {
  assert.fail(`unexpected unhandledRejection: ${error && error.code}`);
});

let pending = 3;
function done() {
  if (--pending === 0) console.log('ok');
}

// Runs at the top level of the main script on purpose: that is where a
// synchronous completion used to be fatal.
{
  const resolver = new dns.promises.Resolver();
  resolver.setServers(['127.0.0.1']);
  const query = resolver.resolve4('example.invalid');
  resolver.cancel();
  query.then(
    () => assert.fail('cancelled query resolved'),
    (error) => {
      assert.equal(error.code, 'ECANCELLED');
      done();
    });
}

{
  const resolver = new dns.Resolver();
  resolver.setServers(['127.0.0.1']);
  let returned = false;
  resolver.resolve4('example.invalid', (error) => {
    assert.ok(returned, 'callback ran synchronously');
    assert.equal(error.code, 'ECANCELLED');
    done();
  });
  resolver.cancel();
  returned = true;
}

{
  let returned = false;
  dns.reverse('127.0.0.1', (error, hostnames) => {
    assert.ok(returned, 'callback ran synchronously');
    assert.ifError(error);
    assert.ok(Array.isArray(hostnames));
    done();
  });
  returned = true;
}
