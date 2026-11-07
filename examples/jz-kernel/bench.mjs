import assert from 'node:assert/strict';
import { compile } from 'jz';

const kernelSource = `
const init = (a, b) => {
  for (let i = 0; i < 16; i++) {
    a[i] = (i + 1) * 0.125
    b[i] = (16 - i) * 0.0625
  }
}

const multiplyMany = (a, b, out, iters) => {
  for (let n = 0; n < iters; n++) {
    for (let r = 0; r < 4; r++) {
      for (let c = 0; c < 4; c++) {
        let s = 0
        for (let k = 0; k < 4; k++) s += a[r * 4 + k] * b[k * 4 + c]
        out[r * 4 + c] = s + n * 0.0000001
      }
    }
    const t = a[0]
    a[0] = out[15]
    a[5] = t + out[10] * 0.000001
  }
}

export let run = (iters) => {
  const a = new Float64Array(16)
  const b = new Float64Array(16)
  const out = new Float64Array(16)
  init(a, b)
  multiplyMany(a, b, out, iters)
  let checksum = 0
  for (let i = 0; i < 16; i++) checksum = (checksum + out[i] * 1000000) | 0
  return checksum
}
`;

function init(a, b) {
  for (let i = 0; i < 16; i += 1) {
    a[i] = (i + 1) * 0.125;
    b[i] = (16 - i) * 0.0625;
  }
}

function multiplyMany(a, b, out, iters) {
  for (let n = 0; n < iters; n += 1) {
    for (let r = 0; r < 4; r += 1) {
      for (let c = 0; c < 4; c += 1) {
        let s = 0;
        for (let k = 0; k < 4; k += 1) s += a[r * 4 + k] * b[k * 4 + c];
        out[r * 4 + c] = s + n * 0.0000001;
      }
    }
    const t = a[0];
    a[0] = out[15];
    a[5] = t + out[10] * 0.000001;
  }
}

function runJs(iters) {
  const a = new Float64Array(16);
  const b = new Float64Array(16);
  const out = new Float64Array(16);
  init(a, b);
  multiplyMany(a, b, out, iters);
  let checksum = 0;
  for (let i = 0; i < 16; i += 1) checksum = (checksum + out[i] * 1000000) | 0;
  return checksum;
}

const wasm = compile(kernelSource);
const mod = new WebAssembly.Module(wasm);
const imports = WebAssembly.Module.imports(mod);
const { exports } = new WebAssembly.Instance(mod);

assert.deepEqual(imports, []);
assert.equal(exports.run(128), runJs(128));

function positiveInt(name, fallback) {
  const value = Number(process.env[name] || fallback);
  assert(Number.isInteger(value) && value > 0, `${name} must be a positive integer`);
  return value;
}

const iterations = positiveInt('ITERATIONS', 200000);
const warmup = positiveInt('WARMUP', 5);
const runs = positiveInt('RUNS', 9);
const now = globalThis.performance && typeof globalThis.performance.now === 'function'
  ? () => globalThis.performance.now()
  : () => Date.now();

function median(values) {
  const sorted = values.slice().sort((a, b) => a - b);
  return sorted[sorted.length >> 1];
}

function measure(fn) {
  for (let i = 0; i < warmup; i += 1) fn(iterations);
  const samples = [];
  let checksum = 0;
  for (let i = 0; i < runs; i += 1) {
    const sampleStart = now();
    checksum = fn(iterations);
    samples.push(now() - sampleStart);
  }
  return {
    checksum,
    medianMs: Number(median(samples).toFixed(3)),
  };
}

const js = measure(runJs);
const jzWasm = measure(exports.run);

assert.equal(jzWasm.checksum, js.checksum);

console.log(JSON.stringify({
  iterations,
  warmup,
  runs,
  imports: imports.length,
  wasmBytes: wasm.length,
  js,
  jzWasm,
}, null, 2));
