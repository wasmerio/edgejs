import assert from 'node:assert/strict';
import { compile } from 'jz';

const kernelSource = `
export let mac = (a, b, c) => {
  let x = c
  for (let i = 0; i < 8; i++) x = x + (a + i) * (b - i)
  return x
}

export let mix = (x, y) => mac(x, y, 3) - mac(y, x, 1)
`;

function macJs(a, b, c) {
  let x = c;
  for (let i = 0; i < 8; i += 1) x += (a + i) * (b - i);
  return x;
}

const wasm = compile(kernelSource);
const mod = new WebAssembly.Module(wasm);

assert.deepEqual(WebAssembly.Module.imports(mod), []);

const { exports } = new WebAssembly.Instance(mod);

for (const [a, b, c] of [[2, 9, 1], [7, 4, 3], [11, -2, 5]]) {
  assert.equal(exports.mac(a, b, c), macJs(a, b, c));
}
assert.equal(exports.mix(4, 7), macJs(4, 7, 3) - macJs(7, 4, 1));

console.log(JSON.stringify({
  imports: 0,
  wasmBytes: wasm.length,
  mac: exports.mac(2, 9, 1),
  mix: exports.mix(4, 7),
}, null, 2));
