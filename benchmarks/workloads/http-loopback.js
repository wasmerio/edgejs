"use strict";

import http from "node:http";

const REQUEST_COUNT = 200;
const RESPONSE_BODY = "edge-http-benchmark";
const RESPONSE_LENGTH = Buffer.byteLength(RESPONSE_BODY);

function makeRequest(port) {
  return new Promise((resolve, reject) => {
    const req = http.get(
      {
        hostname: "127.0.0.1",
        port,
        path: "/",
      },
      (res) => {
        let bytes = 0;

        res.on("data", (chunk) => {
          bytes += chunk.length;
        });

        res.on("end", () => {
          resolve(bytes + res.statusCode);
        });

        res.on("error", reject);
      },
    );

    req.on("error", reject);
  });
}

async function main() {
  let checksum = 0;

  const server = http.createServer((req, res) => {
    res.writeHead(200, {
      "content-type": "text/plain",
      "content-length": RESPONSE_LENGTH,
    });
    res.end(RESPONSE_BODY);
  });

  await new Promise((resolve, reject) => {
    server.listen(0, "127.0.0.1", () => resolve());
    server.on("error", reject);
  });

  const address = server.address();

  if (!address || typeof address !== "object") {
    throw new Error("Failed to determine server address");
  }

  for (let i = 0; i < REQUEST_COUNT; i += 1) {
    checksum += await makeRequest(address.port);
  }

  await new Promise((resolve, reject) => {
    server.close((error) => {
      if (error) {
        reject(error);
        return;
      }
      resolve();
    });
  });

  console.log(String(checksum));
}

main().catch((error) => {
  console.error(error);
  process.exit(1);
});