"use strict";

function parseArgs(argv) {
  const args = {
    benchmark: "console-log",
    edge: null,
    node: null,
    iterations: 20,
    warmups: 3,
  };

  for (let i = 0; i < argv.length; i += 1) {
    const token = argv[i];

    switch (token) {
      case "--benchmark":
        args.benchmark = argv[++i];
        break;
      case "--edge":
        args.edge = argv[++i];
        break;
      case "--node":
        args.node = argv[++i];
        break;
      case "--iterations":
        args.iterations = Number(argv[++i]);
        break;
      case "--warmups":
        args.warmups = Number(argv[++i]);
        break;
      case "--help":
      case "-h":
        args.help = true;
        break;
      default:
        throw new Error(`Unknown argument: ${token}`);
    }
  }

  if (!Number.isInteger(args.iterations) || args.iterations <= 0) {
    throw new Error("--iterations must be a positive integer");
  }

  if (!Number.isInteger(args.warmups) || args.warmups < 0) {
    throw new Error("--warmups must be a non-negative integer");
  }

  return args;
}

function printHelp() {
  console.log(`
Usage:
  node benchmarks/run.js --benchmark console-log --edge ./build-edge/edge --node node

Options:
  --benchmark   Benchmark name (default: console-log)
  --edge        Path to edge executable
  --node        Path to node executable
  --iterations  Measured iterations (default: 20)
  --warmups     Warmup iterations (default: 3)
  --help, -h    Show help
`.trim());
}

module.exports = {
  parseArgs,
  printHelp,
};