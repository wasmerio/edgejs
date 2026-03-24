#!/usr/bin/env node
"use strict";

const { parseArgs, printHelp } = require("./lib/args");
const { runBenchmark, WORKLOADS } = require("./lib/benchmark-runner");

function formatResult(result) {
  const { runtimeName, runtimePath, benchmarkName, warmups, iterations, stats } = result;

  return [
    `runtime:    ${runtimeName}`,
    `path:       ${runtimePath}`,
    `benchmark:  ${benchmarkName}`,
    `warmups:    ${warmups}`,
    `iterations: ${iterations}`,
    `min ms:     ${stats.minMs}`,
    `median ms:  ${stats.medianMs}`,
    `mean ms:    ${stats.meanMs}`,
    `max ms:     ${stats.maxMs}`,
  ].join("\n");
}

function printComparison(results) {
  if (results.length < 2) {
    return;
  }

  const baseline = results[0];
  const baselineMedian = baseline.stats.medianMs;

  console.log("\ncomparison:");
  for (const result of results.slice(1)) {
    const delta = ((result.stats.medianMs - baselineMedian) / baselineMedian) * 100;
    const sign = delta >= 0 ? "+" : "";
    console.log(
      `  ${result.runtimeName} vs ${baseline.runtimeName}: ${sign}${delta.toFixed(2)}% median`
    );
  }
}

function main() {
  let args;

  try {
    args = parseArgs(process.argv.slice(2));
  } catch (error) {
    console.error(`Argument error: ${error.message}`);
    console.error("");
    printHelp();
    process.exit(1);
  }

  if (args.help) {
    printHelp();
    return;
  }

  if (!WORKLOADS[args.benchmark]) {
    console.error(`Unknown benchmark: ${args.benchmark}`);
    console.error(`Available benchmarks: ${Object.keys(WORKLOADS).join(", ")}`);
    process.exit(1);
  }

  const runtimes = [];

  if (args.edge) {
    runtimes.push({ runtimeName: "edge", runtimePath: args.edge });
  }

  if (args.node) {
    runtimes.push({ runtimeName: "node", runtimePath: args.node });
  }

  if (runtimes.length === 0) {
    console.error("Provide at least one runtime with --edge and/or --node");
    process.exit(1);
  }

  const results = [];

  for (const runtime of runtimes) {
    const result = runBenchmark({
      runtimeName: runtime.runtimeName,
      runtimePath: runtime.runtimePath,
      benchmarkName: args.benchmark,
      iterations: args.iterations,
      warmups: args.warmups,
    });

    results.push(result);
    console.log(formatResult(result));
    console.log("");
  }

  printComparison(results);
}

main();