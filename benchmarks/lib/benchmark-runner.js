"use strict";

const { spawnSync } = require("node:child_process");
const path = require("node:path");
const { summarize } = require("./stats");

const WORKLOADS = {
  "console-log": {
    scriptPath: path.resolve(__dirname, "..", "workloads", "console-log.js"),
    expectedStdout: "hello\n",
    argsFor() {
      return [this.scriptPath];
    },
  },
  "empty-startup": {
    scriptPath: path.resolve(__dirname, "..", "workloads", "empty-startup.js"),
    expectedStdout: "",
    argsFor() {
      return [this.scriptPath];
    },
  },
};

function hrtimeMs(startNs, endNs) {
  return Number(endNs - startNs) / 1e6;
}

function runOnce(runtimeName, runtimePath, benchmarkName) {
  const workload = WORKLOADS[benchmarkName];

  if (!workload) {
    throw new Error(`Unknown benchmark: ${benchmarkName}`);
  }

  const start = process.hrtime.bigint();

  const result = spawnSync(runtimePath, workload.argsFor(runtimePath), {
    encoding: "utf8",
    stdio: ["ignore", "pipe", "pipe"],
  });

  const end = process.hrtime.bigint();
  const elapsedMs = hrtimeMs(start, end);

  if (result.error) {
    throw new Error(
      `[${runtimeName}] Failed to execute ${runtimePath}: ${result.error.message}`
    );
  }

  if (result.status !== 0) {
    throw new Error(
      `[${runtimeName}] Non-zero exit code ${result.status}\n` +
        `stderr:\n${result.stderr || "(empty)"}`
    );
  }

  if (result.stdout !== workload.expectedStdout) {
    throw new Error(
      `[${runtimeName}] Unexpected stdout.\n` +
        `Expected: ${JSON.stringify(workload.expectedStdout)}\n` +
        `Received: ${JSON.stringify(result.stdout)}`
    );
  }

  return elapsedMs;
}

function runBenchmark({ runtimeName, runtimePath, benchmarkName, iterations, warmups }) {
  for (let i = 0; i < warmups; i += 1) {
    runOnce(runtimeName, runtimePath, benchmarkName);
  }

  const samplesMs = [];

  for (let i = 0; i < iterations; i += 1) {
    samplesMs.push(runOnce(runtimeName, runtimePath, benchmarkName));
  }

  return {
    runtimeName,
    runtimePath,
    benchmarkName,
    warmups,
    iterations,
    stats: summarize(samplesMs),
    samplesMs,
  };
}

module.exports = {
  runBenchmark,
  WORKLOADS,
};
