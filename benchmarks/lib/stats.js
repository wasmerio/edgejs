"use strict";

function sortNumeric(values) {
  return [...values].sort((a, b) => a - b);
}

function round(value) {
  return Number(value.toFixed(3));
}

function median(values) {
  const sorted = sortNumeric(values);
  const middle = Math.floor(sorted.length / 2);

  if (sorted.length % 2 === 0) {
    return (sorted[middle - 1] + sorted[middle]) / 2;
  }

  return sorted[middle];
}

function mean(values) {
  const total = values.reduce((sum, value) => sum + value, 0);
  return total / values.length;
}

function summarize(samplesMs) {
  if (!samplesMs.length) {
    throw new Error("Cannot summarize an empty sample set");
  }

  const sorted = sortNumeric(samplesMs);

  return {
    count: samplesMs.length,
    minMs: round(sorted[0]),
    maxMs: round(sorted[sorted.length - 1]),
    medianMs: round(median(sorted)),
    meanMs: round(mean(sorted)),
  };
}

module.exports = {
  summarize,
};