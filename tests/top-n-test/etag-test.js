'use strict';

const assert = require('node:assert/strict');
const fs = require('node:fs');

const etag = require('etag');

// Generate etag from a string
const tag1 = etag('hello world');
assert.equal(typeof tag1, 'string');
assert.ok(tag1.startsWith('"'), 'strong etag should start with quote');
assert.ok(tag1.endsWith('"'), 'strong etag should end with quote');

// Generate etag from a Buffer
const tag2 = etag(Buffer.from('hello world'));
assert.equal(typeof tag2, 'string');
assert.ok(tag2.startsWith('"'));
assert.ok(tag2.endsWith('"'));

// Same content should produce the same etag
assert.equal(tag1, tag2);

// Different content should produce different etags
const tag3 = etag('different content');
assert.notEqual(tag1, tag3);

// Generate a weak etag
const weakTag = etag('hello world', { weak: true });
assert.equal(typeof weakTag, 'string');
assert.ok(weakTag.startsWith('W/"'), 'weak etag should start with W/"');
assert.ok(weakTag.endsWith('"'));

// Generate etag from fs.Stats
const stat = fs.statSync(__filename);
const statTag = etag(stat);
assert.equal(typeof statTag, 'string');
assert.ok(statTag.startsWith('"') || statTag.startsWith('W/"'), 'stat etag should be quoted');
assert.ok(statTag.endsWith('"'));

console.log('etag-test:ok');
