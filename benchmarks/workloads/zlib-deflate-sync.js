"use strict";

import { deflateSync } from "node:zlib";

const PAYLOAD = JSON.stringify({
  meta: {
    name: "edge-zlib-benchmark",
    version: 1,
    flags: ["alpha", "beta", "gamma"],
  },
  items: Array.from({ length: 200 }, (_, index) => ({
    id: index,
    label: `item-${index}`,
    active: index % 2 === 0,
    values: [index, index * 2, index * 3, index * 4],
  })),
});

let checksum = 0;
let totalCompressedLength = 0;

for (let i = 0; i < 250; i += 1) {
  const compressed = deflateSync(PAYLOAD);

  checksum +=
    compressed[0] +
    compressed[1] +
    compressed[compressed.length - 1] +
    compressed.length;

  totalCompressedLength += compressed.length;
}

console.log(`${checksum}:${totalCompressedLength}`);