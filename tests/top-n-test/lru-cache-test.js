'use strict';

const assert = require('node:assert/strict');

(async () => {
  const mod = require('lru-cache');
  const LRUCache = mod.LRUCache || mod.default || mod;

  // Create a cache with a max size of 3
  const cache = new LRUCache({ max: 3 });

  // Basic set/get/has
  cache.set('a', 1);
  cache.set('b', 2);
  cache.set('c', 3);
  assert.equal(cache.get('a'), 1);
  assert.equal(cache.get('b'), 2);
  assert.equal(cache.get('c'), 3);
  assert.equal(cache.has('a'), true);
  assert.equal(cache.has('nonexistent'), false);
  assert.equal(cache.get('nonexistent'), undefined);

  // Size reflects the number of entries
  assert.equal(cache.size, 3);

  // Eviction: adding a 4th item should evict the least recently used
  // 'a' was accessed most recently (via get above), so 'b' should be evicted
  // Actually after the gets above, order is c(newest set), then b(get), then a(get last)
  // Wait - LRU evicts the least recently used. After set a,b,c then get a, get b, get c:
  // The access order is a, b, c with c most recent. So adding d evicts a.
  cache.set('d', 4);
  assert.equal(cache.size, 3, 'cache should not exceed max size');
  assert.equal(cache.has('d'), true);
  assert.equal(cache.get('d'), 4);
  // 'a' was the least recently used after the gets (a was got first, then b, then c)
  assert.equal(cache.has('a'), false, 'least recently used entry should be evicted');

  // delete removes a specific key
  cache.set('e', 5);
  assert.equal(cache.has('e'), true);
  assert.equal(cache.delete('e'), true);
  assert.equal(cache.has('e'), false);

  // peek returns value without updating recency
  cache.clear();
  cache.set('x', 10);
  cache.set('y', 20);
  cache.set('z', 30);
  // peek 'x' does NOT make it recently used
  const peeked = cache.peek('x');
  assert.equal(peeked, 10);
  // Now add a new item; 'x' should still be evicted since peek didn't refresh it
  cache.set('w', 40);
  assert.equal(cache.has('x'), false, 'peeked item should still be evicted as LRU');
  assert.equal(cache.has('w'), true);

  // forEach iterates over entries
  cache.clear();
  cache.set('k1', 100);
  cache.set('k2', 200);
  const collected = {};
  cache.forEach((value, key) => {
    collected[key] = value;
  });
  assert.equal(collected['k1'], 100);
  assert.equal(collected['k2'], 200);

  // clear empties the cache
  cache.clear();
  assert.equal(cache.size, 0);

  // TTL support
  const ttlCache = new LRUCache({ max: 10, ttl: 50 });
  ttlCache.set('temp', 'value');
  assert.equal(ttlCache.get('temp'), 'value');

  // Wait for TTL to expire
  await new Promise((resolve) => setTimeout(resolve, 100));
  assert.equal(ttlCache.get('temp'), undefined, 'entry should expire after TTL');

  console.log('lru-cache-test:ok');
})().catch((err) => {
  console.error('lru-cache-test:fail');
  console.error(err && err.stack ? err.stack : String(err));
  process.exit(1);
});
