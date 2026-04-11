import { rollingHash } from "./hash.js";
import { TOKENS } from "./tokens.js";

export function graphB(seed) {
  let acc = 0;

  for (let i = TOKENS.length - 1; i >= 0; i -= 1) {
    const token = `${TOKENS[i]}:${(seed * (i + 3)) % 19}`;
    acc = (acc ^ rollingHash(token, seed + i * 13)) >>> 0;
  }

  return acc;
}
