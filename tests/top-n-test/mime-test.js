'use strict';

const assert = require('node:assert/strict');
const mime = require('mime');

try {
  // Lookup common MIME types by extension
  assert.equal(mime.getType('html'), 'text/html');
  assert.equal(mime.getType('js'), 'application/javascript');
  assert.equal(mime.getType('json'), 'application/json');
  assert.equal(mime.getType('png'), 'image/png');
  assert.equal(mime.getType('css'), 'text/css');
  assert.equal(mime.getType('txt'), 'text/plain');

  // Lookup with full filename
  assert.equal(mime.getType('photo.jpg'), 'image/jpeg');
  assert.equal(mime.getType('archive.tar.gz'), 'application/gzip');

  // Reverse lookup: get extension from MIME type
  assert.equal(mime.getExtension('text/html'), 'html');
  assert.equal(mime.getExtension('application/json'), 'json');
  assert.equal(mime.getExtension('image/png'), 'png');

  // Define custom MIME types using mime.define
  mime.define({ 'text/x-custom': ['xcustom'] });
  assert.equal(mime.getType('xcustom'), 'text/x-custom');
  assert.equal(mime.getExtension('text/x-custom'), 'xcustom');

  // Unknown extension returns null
  assert.equal(mime.getType('xyz-unknown-ext-999'), null);

  console.log('mime-test:ok');
} catch (err) {
  console.error('mime-test:fail');
  console.error(err && err.stack ? err.stack : String(err));
  process.exit(1);
}
