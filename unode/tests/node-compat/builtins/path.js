'use strict';

function join() {
  const parts = [];
  for (let i = 0; i < arguments.length; i += 1) {
    const p = String(arguments[i]).replace(/\/+$/, '');
    if (p) parts.push(p);
  }
  return parts.join('/').replace(/\/+/g, '/');
}

function resolve() {
  let result = join.apply(null, arguments);
  if (result && result.indexOf('/') !== 0) {
    const cwd = typeof process !== 'undefined' && process.cwd ? process.cwd() : '.';
    result = join(cwd, result);
  }
  return result.replace(/\/+/g, '/').replace(/\/$/, '') || '/';
}

function relative(from, to) {
  from = resolve(from).replace(/\/$/, '') || '/';
  to = resolve(to).replace(/\/$/, '') || '/';
  if (from === to) return '';
  const a = from.split('/').filter(Boolean);
  const b = to.split('/').filter(Boolean);
  let i = 0;
  while (i < a.length && i < b.length && a[i] === b[i]) i++;
  const up = a.length - i;
  const segs = (new Array(up)).fill('..').concat(b.slice(i));
  return segs.join('/');
}

function dirname(p) {
  p = String(p);
  const last = p.lastIndexOf('/');
  if (last <= 0) return last === 0 ? '/' : '.';
  return p.slice(0, last) || '/';
}

function basename(p, ext) {
  p = String(p);
  let last = p.lastIndexOf('/');
  let name = last === -1 ? p : p.slice(last + 1);
  if (ext !== undefined && ext !== '' && name.endsWith(ext)) name = name.slice(0, name.length - String(ext).length);
  return name || (last === 0 ? '' : (last === -1 ? p : ''));
}

module.exports = { join, resolve, relative, dirname, basename };
