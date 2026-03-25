"use strict";

const PAYLOAD = JSON.stringify({
  meta: {
    name: "edge-benchmark",
    version: 1,
    flags: ["alpha", "beta", "gamma"],
  },
  items: Array.from({ length: 100 }, (_, index) => ({
    id: index,
    label: `item-${index}`,
    values: [index, index * 2, index * 3],
    active: index % 2 === 0,
  })),
});

let checksum = 0;
let totalLength = 0;

for (let i = 0; i < 250; i += 1) {
  const parsed = JSON.parse(PAYLOAD);

  checksum +=
    parsed.items[10].id +
    parsed.items[90].values[2] +
    parsed.meta.version;

  totalLength += JSON.stringify(parsed).length;
}

console.log(`${checksum}:${totalLength}`);
