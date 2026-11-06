'use strict';

// Public vm regressions for the N-API realm integration. Run with
// --experimental-vm-modules on Node and both Edge providers.
const assert = require('node:assert/strict');
const vm = require('node:vm');
const { createHash } = require('node:crypto');

async function check(name, run) {
  try {
    await run();
  } catch (error) {
    error.message = `${name}: ${error.message}`;
    throw error;
  }
}

async function main() {
  await check('isolated globals and intrinsics', () => {
    const first = { seed: 11 };
    const second = { seed: 22 };
    assert.equal(vm.createContext(first), first);
    assert.equal(vm.createContext(first), first);
    vm.createContext(second);
    assert.equal(vm.isContext(first), true);
    assert.equal(vm.isContext({}), false);

    const names = ['Object', 'Array', 'Function', 'Promise', 'Error',
                   'Uint8Array', 'DataView', 'ArrayBuffer'];
    for (const name of names) {
      const firstIntrinsic = vm.runInContext(name, first);
      assert.notEqual(firstIntrinsic, globalThis[name], name);
      assert.notEqual(firstIntrinsic, vm.runInContext(name, second), name);
    }
    assert.equal(vm.runInContext('globalThis.seed += 1; var written = 33; seed', first), 12);
    assert.equal(first.seed, 12);
    assert.equal(first.written, 33);
    assert.equal(vm.runInContext('typeof written', second), 'undefined');
    assert.equal(second.seed, 22);
    first.seed = 40;
    assert.equal(vm.runInContext('seed', first), 40);

    const object = vm.runInContext('({ answer: seed })', first);
    assert.equal(object.answer, 40);
    assert.equal(Object.getPrototypeOf(object), vm.runInContext('Object.prototype', first));
    const realmError = vm.runInContext('new TypeError("realm error")', first);
    assert.equal(realmError instanceof vm.runInContext('TypeError', first), true);
    assert.equal(realmError instanceof TypeError, false);
  });

  await check('script reuse and nested execution', () => {
    const first = vm.createContext({ label: 'first' });
    const second = vm.createContext({ label: 'second' });
    const script = new vm.Script('({ label, array: [] })');
    const firstResult = script.runInContext(first);
    const secondResult = script.runInContext(second);
    assert.equal(firstResult.label, 'first');
    assert.equal(secondResult.label, 'second');
    assert.equal(firstResult.array instanceof vm.runInContext('Array', first), true);
    assert.equal(secondResult.array instanceof vm.runInContext('Array', second), true);

    first.runOther = () => vm.runInContext('({ label, object: {} })', second);
    const nested = vm.runInContext('({ before: label, inner: runOther(), after: label, array: [] })', first);
    assert.equal(nested.before, 'first');
    assert.equal(nested.after, 'first');
    assert.equal(nested.inner.label, 'second');
    assert.equal(nested.inner.object instanceof vm.runInContext('Object', second), true);
    assert.equal(nested.array instanceof vm.runInContext('Array', first), true);
    assert.equal(vm.runInThisContext('Object'), Object);
    assert.throws(() => vm.runInContext('throw new Error("nested failure")', second),
                  { message: 'nested failure' });
    assert.equal(vm.runInThisContext('Array'), Array);
  });

  await check('native functions keep their creation realm', () => {
    const encoder = new TextEncoder();
    const context = vm.createContext({
      encode: encoder.encode.bind(encoder),
      Buffer,
      createHash,
    });
    const values = vm.runInContext(`
      (() => {
        let error;
        try { createHash('edge-realm-unsupported-digest'); } catch (caught) { error = caught; }
        return { encoded: encode('realm'), buffer: Buffer.alloc(3), error, Object, Uint8Array, Error };
      })()
    `, context);
    assert.equal(Object.getPrototypeOf(values.encoded), Uint8Array.prototype);
    assert.equal(values.encoded instanceof values.Uint8Array, false);
    assert.deepEqual(Array.from(values.encoded), [114, 101, 97, 108, 109]);
    assert.equal(Object.getPrototypeOf(values.buffer), Buffer.prototype);
    assert.equal(values.buffer instanceof values.Uint8Array, false);
    assert.equal(values.error instanceof Error, true);
    assert.equal(values.error instanceof values.Error, false);
    assert.equal(vm.runInThisContext('Object'), Object);
  });

  await check('native consumers read cross-realm view slots', () => {
    const view = vm.runInNewContext(`
      const view = new Uint8Array([0, 97, 98, 99, 0]).subarray(1, 4);
      for (const name of ['buffer', 'byteOffset', 'byteLength', 'length']) {
        Object.defineProperty(view, name, { get() { throw new Error('spoofed ' + name); } });
      }
      view;
    `);
    assert.equal(createHash('sha256').update(view).digest('hex'),
                 'ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad');
  });

  await check('compileFunction parsing contexts', () => {
    const context = vm.createContext({ label: 'compiled realm', base: 20 });
    const compiled = vm.compileFunction(
      'return { label, sum: base + value, object: {}, array: new Uint8Array([value]) };',
      ['value'], { parsingContext: context });
    const value = compiled(22);
    assert.equal(value.label, 'compiled realm');
    assert.equal(value.sum, 42);
    assert.equal(Object.getPrototypeOf(compiled), vm.runInContext('Function.prototype', context));
    assert.equal(Object.getPrototypeOf(value.object), vm.runInContext('Object.prototype', context));
    assert.equal(Object.getPrototypeOf(value.array), vm.runInContext('Uint8Array.prototype', context));

    context.principalFunction = vm.compileFunction('return { object: {}, array: [] };');
    const principalValue = vm.runInContext('principalFunction()', context);
    assert.equal(Object.getPrototypeOf(principalValue.object), Object.prototype);
    assert.equal(Object.getPrototypeOf(principalValue.array), Array.prototype);
    assert.throws(() => vm.compileFunction('', [], { parsingContext: {} }),
                  { code: 'ERR_INVALID_ARG_TYPE' });

    const source = 'return { label, array: new Uint8Array([value]) };';
    const cacheProducer = vm.compileFunction(source, ['value'],
      { parsingContext: context, produceCachedData: true });
    assert.equal(Buffer.isBuffer(cacheProducer.cachedData), true);
    assert.ok(cacheProducer.cachedData.length > 0);
    const cacheContext = vm.createContext({ label: 'cached realm' });
    const cachedFunction = vm.compileFunction(source, ['value'],
      { parsingContext: cacheContext, cachedData: cacheProducer.cachedData });
    assert.equal(cachedFunction.cachedDataRejected, false);
    const cachedValue = cachedFunction(29);
    assert.equal(cachedValue.label, 'cached realm');
    assert.equal(cachedValue.array[0], 29);
    assert.equal(Object.getPrototypeOf(cachedFunction),
                 vm.runInContext('Function.prototype', cacheContext));
    assert.equal(Object.getPrototypeOf(cachedValue.array),
                 vm.runInContext('Uint8Array.prototype', cacheContext));
  });

  await check('functions retain their realm', () => {
    let context = vm.createContext({ secret: 73 });
    const retained = vm.runInContext('() => ({ secret, array: [], Array })', context);
    context = null;
    // Creating other realms exercises providers that reap collected realm
    // records when a new context is made. The function must retain its realm.
    for (let i = 0; i < 16; i++) vm.runInNewContext('({ temporary: true })');
    const value = retained();
    assert.equal(value.secret, 73);
    assert.notEqual(value.Array, Array);
    assert.equal(Object.getPrototypeOf(value.array), value.Array.prototype);
    assert.equal(vm.runInThisContext('Array'), Array);
  });

  await check('promise jobs retain their realm', async () => {
    let complete;
    const completion = new Promise((resolve) => { complete = resolve; });
    let context = vm.createContext({ seed: 91, complete });
    vm.runInContext('Promise.resolve().then(() => complete({ array: [seed], Array }))', context);
    context = null;
    // Only the queued job retains realm objects before it invokes complete.
    for (let i = 0; i < 16; i++) vm.createContext({ temporary: i });
    const value = await completion;
    assert.equal(value.array[0], 91);
    assert.notEqual(value.Array, Array);
    assert.equal(Object.getPrototypeOf(value.array), value.Array.prototype);
  });

  await check('modules use their creation realm', async () => {
    assert.equal(typeof vm.SourceTextModule, 'function');
    const context = vm.createContext({ seed: 100 });
    const dependency = new vm.SourceTextModule('export const value = new Uint8Array([seed]);',
                                               { context });
    const module = new vm.SourceTextModule(`
      import { value } from 'dependency';
      await Promise.resolve();
      export { value };
      export const object = {};
      export const getArray = () => [seed];
    `, { context });
    await module.link((specifier) => {
      assert.equal(specifier, 'dependency');
      return dependency;
    });
    await module.evaluate();
    assert.equal(module.namespace.value[0], 100);
    assert.equal(Object.getPrototypeOf(module.namespace.value),
                 vm.runInContext('Uint8Array.prototype', context));
    assert.equal(Object.getPrototypeOf(module.namespace.object),
                 vm.runInContext('Object.prototype', context));
    assert.equal(Object.getPrototypeOf(module.namespace.getArray()),
                 vm.runInContext('Array.prototype', context));

    const rejected = new vm.SourceTextModule(
      'globalThis.beforeError = 1; throw new Error("expected module error");', { context });
    await rejected.link(() => {});
    await assert.rejects(rejected.evaluate(), { message: 'expected module error' });
    assert.equal(context.beforeError, 1);

    const otherContext = vm.createContext({});
    const otherModule = new vm.SourceTextModule('import "dependency";', { context: otherContext });
    await assert.rejects(otherModule.link(() => dependency),
                         { code: 'ERR_VM_MODULE_DIFFERENT_CONTEXT' });

    const source = 'export const value = [seed];';
    const cacheProducer = new vm.SourceTextModule(source, { context });
    const cachedData = cacheProducer.createCachedData();
    assert.equal(Buffer.isBuffer(cachedData), true);
    assert.ok(cachedData.length > 0);
    const cacheContext = vm.createContext({ seed: 123 });
    const cachedModule = new vm.SourceTextModule(source, { context: cacheContext, cachedData });
    await cachedModule.link(() => {});
    await cachedModule.evaluate();
    assert.equal(cachedModule.namespace.value[0], 123);
    assert.equal(Object.getPrototypeOf(cachedModule.namespace.value),
                 vm.runInContext('Array.prototype', cacheContext));
  });

  await check('native ESM and CommonJS facades', async () => {
    const bufferModule = await import('node:buffer');
    assert.equal(bufferModule.Buffer, Buffer);
    assert.equal(bufferModule.default.Buffer, Buffer);

    const fs = require('node:fs');
    const path = require('node:path');
    const { pathToFileURL } = require('node:url');
    const directory = fs.mkdtempSync(path.join(require('node:os').tmpdir(), 'edge-napi-realms-'));
    try {
      const filename = path.join(directory, 'facade.cjs');
      fs.writeFileSync(filename, 'exports.answer = 42; exports.Object = Object;\n');
      const commonjsModule = await import(pathToFileURL(filename).href);
      assert.equal(commonjsModule.answer, 42);
      assert.equal(commonjsModule.default.Object, Object);
      assert.equal(commonjsModule.default, require(filename));
    } finally {
      fs.rmSync(directory, { recursive: true, force: true });
    }
  });

  await check('synthetic evaluation preserves host sandbox writes', async () => {
    for (const rejects of [false, true]) {
      const context = vm.createContext({ value: 1 });
      const synthetic = new vm.SyntheticModule([], function () {
        context.value = 2;
        if (rejects) throw new Error('expected synthetic error');
      }, { context });
      await synthetic.link(() => {});
      if (rejects) {
        await assert.rejects(synthetic.evaluate(), { message: 'expected synthetic error' });
      } else {
        await synthetic.evaluate();
      }
      assert.equal(context.value, 2);
    }

    const context = vm.createContext({ value: 1, sourceValue: 0 });
    const nested = new vm.SyntheticModule([], function () {
      context.value += 1;
    }, { context });
    await nested.link(() => {});
    let nestedEvaluation;
    const synthetic = new vm.SyntheticModule([], function () {
      assert.equal(context.sourceValue, 10);
      context.value = 2;
      nestedEvaluation = nested.evaluate();
      assert.equal(context.value, 3);
    }, { context });
    const source = new vm.SourceTextModule('globalThis.sourceValue = 10;', { context });
    const root = new vm.SourceTextModule(`
      import 'source';
      import 'synthetic';
      export const value = globalThis.value;
      export const sourceValue = globalThis.sourceValue;
    `, { context });
    await root.link((name) => name === 'source' ? source : synthetic);
    await root.evaluate();
    await nestedEvaluation;
    assert.equal(context.value, 3);
    assert.equal(root.namespace.value, 3);
    assert.equal(context.sourceValue, 10);
    assert.equal(root.namespace.sourceValue, 10);
  });

  await check('import-meta hook preserves host sandbox writes', async () => {
    for (const rejects of [false, true]) {
      const context = vm.createContext({ value: 1, sourceValue: 0 });
      const module = new vm.SourceTextModule(`
        globalThis.sourceValue = 10;
        export const value = import.meta.x;
        export const after = globalThis.value;
      `, {
        context,
        initializeImportMeta(meta) {
          assert.equal(context.sourceValue, 10);
          context.value = 2;
          if (rejects) throw new Error('expected meta error');
          meta.x = 3;
        },
      });
      await module.link(() => {});
      if (rejects) {
        await assert.rejects(module.evaluate(), { message: 'expected meta error' });
      } else {
        await module.evaluate();
        assert.equal(module.namespace.value, 3);
        assert.equal(module.namespace.after, 2);
      }
      assert.equal(context.value, 2);
      assert.equal(context.sourceValue, 10);
    }
  });

  console.log('NAPI_REALMS_OK');
}

let completed = false;
process.on('exit', () => {
  if (!completed) process.exitCode = 1;
});
main().then(() => {
  completed = true;
}, (error) => {
  console.error(error);
  process.exitCode = 1;
});
