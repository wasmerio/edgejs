'use strict';

const assert = require('node:assert/strict');
const { Readable } = require('node:stream');

const mergeStream = require('merge-stream');

(async () => {
  // Create two readable streams that push data
  const stream1 = new Readable({
    read() {
      this.push('hello');
      this.push(null);
    }
  });

  const stream2 = new Readable({
    read() {
      this.push('world');
      this.push(null);
    }
  });

  // Merge them together
  const merged = mergeStream(stream1, stream2);
  assert.ok(merged, 'mergeStream should return a stream');
  assert.equal(typeof merged.pipe, 'function', 'merged stream should be pipeable');

  // Collect all data from the merged stream
  const chunks = [];
  await new Promise((resolve, reject) => {
    merged.on('data', (chunk) => chunks.push(chunk.toString()));
    merged.on('end', resolve);
    merged.on('error', reject);
  });

  assert.ok(chunks.includes('hello'), 'should contain data from stream1');
  assert.ok(chunks.includes('world'), 'should contain data from stream2');

  // Test adding a stream after creation
  const stream3 = new Readable({
    read() {
      this.push('added');
      this.push(null);
    }
  });

  const merged2 = mergeStream();
  merged2.add(stream3);

  const chunks2 = [];
  await new Promise((resolve, reject) => {
    merged2.on('data', (chunk) => chunks2.push(chunk.toString()));
    merged2.on('end', resolve);
    merged2.on('error', reject);
  });

  assert.ok(chunks2.includes('added'), 'should contain data from dynamically added stream');

  // Test that mergeStream with no args returns a valid stream
  const emptyMerge = mergeStream();
  assert.equal(typeof emptyMerge.pipe, 'function', 'empty merge should still be a stream');
  assert.equal(typeof emptyMerge.add, 'function', 'should have an add method');

  console.log('merge-stream-test:ok');
})().catch((err) => {
  console.error('merge-stream-test:fail');
  console.error(err && err.stack ? err.stack : String(err));
  process.exit(1);
});
