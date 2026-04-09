'use strict';

const assert = require('node:assert/strict');

(async () => {
  const { of, from, firstValueFrom, lastValueFrom, Subject, map, filter, pipe, take, toArray } = require('rxjs');

  // of() creates an observable from arguments
  const val = await lastValueFrom(of(10, 20, 30));
  assert.equal(val, 30);

  // from() creates an observable from an array
  const first = await firstValueFrom(from([5, 10, 15]));
  assert.equal(first, 5);

  // pipe with map operator
  const mapped = await lastValueFrom(of(3).pipe(map(x => x * x)));
  assert.equal(mapped, 9);

  // pipe with filter and map
  const result = await lastValueFrom(
    from([1, 2, 3, 4, 5, 6]).pipe(
      filter(x => x % 2 === 0),
      map(x => x * 10),
      take(2),
    )
  );
  assert.equal(result, 40); // last of [20, 40]

  // Collect all values via subscribe
  const collected = [];
  await new Promise((resolve) => {
    of('a', 'b', 'c').subscribe({
      next: (v) => collected.push(v),
      complete: resolve,
    });
  });
  assert.deepEqual(collected, ['a', 'b', 'c']);

  // Subject: acts as both observable and observer
  const subject = new Subject();
  const subValues = [];
  const subscription = subject.subscribe(v => subValues.push(v));
  subject.next(1);
  subject.next(2);
  subject.next(3);
  subject.complete();
  subscription.unsubscribe();
  assert.deepEqual(subValues, [1, 2, 3]);

  // toArray collects all emissions into one array
  const arr = await lastValueFrom(from([10, 20, 30]).pipe(toArray()));
  assert.deepEqual(arr, [10, 20, 30]);

  // firstValueFrom rejects on empty observable
  try {
    await firstValueFrom(from([]));
    assert.fail('should have thrown on empty observable');
  } catch (e) {
    assert.ok(e.message.includes('no elements'));
  }

  console.log('rxjs-test:ok');
})().catch((err) => {
  console.error('rxjs-test:fail');
  console.error(err && err.stack ? err.stack : String(err));
  process.exit(1);
});
