import { graphA } from "./graph-a.js";
import { graphB } from "./graph-b.js";
import { TOKENS } from "./tokens.js";

export function runDependencyWorkload(seed) {
  const partA = graphA(seed);
  const partB = graphB(seed + 7);
  const tokenMix = TOKENS[(seed * 5) % TOKENS.length].length * 131;
  return (partA + partB + tokenMix) >>> 0;
}
