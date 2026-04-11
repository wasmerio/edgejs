"use strict";

import { runDependencyWorkload } from "./fixtures/dependency-heavy-package/index.js";

const ITERATIONS = 2500;
let checksum = 0;

for (let i = 0; i < ITERATIONS; i += 1) {
  checksum = (checksum + runDependencyWorkload(i)) % 1000000007;
}

console.log(String(checksum));
