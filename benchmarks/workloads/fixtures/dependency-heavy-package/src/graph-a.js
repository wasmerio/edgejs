import { rollingHash } from "./hash.js";
import { normalizeToken } from "./normalize.js";
import { TOKENS } from "./tokens.js";

export function graphA(seed) {
  let acc = 0;

  for (let i = 0; i < TOKENS.length; i += 1) {
    const normalized = normalizeToken(`${TOKENS[i]}-${(seed + i) % 17}`);
    acc = (acc + rollingHash(normalized, seed + i)) >>> 0;
  }

  return acc;
}
