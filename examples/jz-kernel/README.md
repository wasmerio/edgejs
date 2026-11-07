# JZ kernel example

[`jz`](https://www.npmjs.com/package/jz) compiles a JavaScript-subset numeric kernel to standard WebAssembly. EdgeJS loads it through `WebAssembly.Module` / `WebAssembly.Instance`.

```bash
npm ci
edge bench.mjs
```

`bench.mjs` runs a small `Float64Array` 4x4 matrix kernel through one compiled export and prints checksum, import count, byte size, and median timings for raw JS vs JZ WASM. Tune with `ITERATIONS`, `WARMUP`, `RUNS`.

`index.mjs` is a one-shot smoke check: `edge index.mjs`.

JZ fits hot numeric, DSP, parser, and typed-array kernels. It is not a JavaScript runtime.
