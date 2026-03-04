'use strict';

function getSourceMapsSupport() {
  return { enabled: true };
}

function findSourceMap() {
  return undefined;
}

function getSourceLine() {
  return undefined;
}

module.exports = {
  getSourceMapsSupport,
  findSourceMap,
  getSourceLine,
};
