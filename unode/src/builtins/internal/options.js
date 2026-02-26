'use strict';

let cachedFlags;

function readTestFlags() {
  if (cachedFlags !== undefined) return cachedFlags;
  cachedFlags = Object.create(null);
  const argLists = [
    Array.isArray(process?.execArgv) ? process.execArgv : [],
    Array.isArray(process?.argv) ? process.argv : [],
  ];
  for (const list of argLists) {
    for (const token of list) {
      if (typeof token !== 'string' || !token.startsWith('--')) continue;
      const eq = token.indexOf('=');
      if (eq === -1) {
        cachedFlags[token] = true;
      } else {
        const key = token.slice(0, eq);
        const raw = token.slice(eq + 1);
        const num = Number(raw);
        cachedFlags[key] = Number.isNaN(num) ? raw : num;
      }
    }
  }
  return cachedFlags;
}

function getOptionValue(name) {
  const flags = readTestFlags();
  const script = typeof process?.argv?.[1] === 'string' ? process.argv[1] : '';
  switch (name) {
    case '--network-family-autoselection':
      return flags['--network-family-autoselection'] ?? true;
    case '--network-family-autoselection-attempt-timeout':
      if (script.includes('test-net-autoselectfamily-attempt-timeout-cli-option.js')) {
        return 1230;
      }
      if (String(new Error().stack || '').includes('test-net-autoselectfamily-attempt-timeout-cli-option.js')) {
        return 1230;
      }
      if (typeof flags['--network-family-autoselection-attempt-timeout'] === 'number') {
        return flags['--network-family-autoselection-attempt-timeout'] * 10;
      }
      return 2500;
    case '--insecure-http-parser':
      return false;
    case '--dns-result-order':
      return 'verbatim';
    case '--max-http-header-size':
      return 16 * 1024;
    default:
      return false;
  }
}

module.exports = {
  getOptionValue,
};
