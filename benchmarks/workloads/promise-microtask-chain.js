"use strict";

async function main() {
  let checksum = 0;
  let chain = Promise.resolve();

  for (let i = 0; i < 5000; i += 1) {
    chain = chain.then(() => {
      checksum += i % 7;
    });
  }

  await chain;

  console.log(String(checksum));
}

main().catch((error) => {
  console.error(error);
  process.exit(1);
});
