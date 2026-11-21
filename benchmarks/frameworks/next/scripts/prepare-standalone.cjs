'use strict';

const fs = require('node:fs');
const path = require('node:path');

const projectRoot = path.resolve(__dirname, '..');
const standaloneDir = path.join(projectRoot, '.next', 'standalone');
const staticSrc = path.join(projectRoot, '.next', 'static');
const staticDest = path.join(standaloneDir, '.next', 'static');
const publicSrc = path.join(projectRoot, 'public');
const publicDest = path.join(standaloneDir, 'public');

function copyRecursive(source, destination) {
  fs.mkdirSync(destination, { recursive: true });
  for (const entry of fs.readdirSync(source, { withFileTypes: true })) {
    const from = path.join(source, entry.name);
    const to = path.join(destination, entry.name);
    if (entry.isDirectory()) {
      copyRecursive(from, to);
      continue;
    }
    fs.copyFileSync(from, to);
  }
}

if (!fs.existsSync(standaloneDir)) {
  throw new Error(`missing Next standalone output: ${standaloneDir}`);
}

if (fs.existsSync(staticSrc)) {
  copyRecursive(staticSrc, staticDest);
}

if (fs.existsSync(publicSrc)) {
  copyRecursive(publicSrc, publicDest);
}

console.log('prepared Next.js standalone layout');
