"use strict";

const TOKENS = Array.from({ length: 200 }, (_, index) => `segment-${index}`).join("|");
const PARTS = TOKENS.split("|");
const TARGET_A = "segment-42";
const TARGET_B = "segment-142";

let checksum = 0;
let totalParts = 0;

for (let iteration = 0; iteration < 5000; iteration += 1) {
  const split = TOKENS.split("|");
  totalParts += split.length;

  for (let index = 0; index < split.length; index += 1) {
    const value = split[index];

    if (value === TARGET_A) {
      checksum += index;
    }

    if (value < TARGET_B) {
      checksum += value.length & 3;
    }
  }
}

console.log(`${checksum}:${totalParts}:${PARTS.length}`);