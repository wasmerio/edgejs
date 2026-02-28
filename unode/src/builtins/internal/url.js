'use strict';

const NativeURL = globalThis.URL;
const NativeURLSearchParams = globalThis.URLSearchParams;
const { Buffer } = require('buffer');
const path = require('path');
const {
  codes: {
    ERR_INVALID_ARG_TYPE,
    ERR_INVALID_ARG_VALUE,
    ERR_INVALID_FILE_URL_HOST,
    ERR_INVALID_FILE_URL_PATH,
    ERR_INVALID_URL_SCHEME,
  },
} = require('internal/errors');

const bindingUrl = internalBinding('url');
const isWindows = process.platform === 'win32';
const FORWARD_SLASH = /\//g;

const unsafeProtocol = new Set(['javascript', 'javascript:']);
const hostlessProtocol = new Set(['javascript', 'javascript:']);
const slashedProtocol = new Set(['http', 'http:', 'https', 'https:', 'ftp', 'ftp:', 'file', 'file:', 'ws', 'ws:', 'wss', 'wss:']);

class SimpleURLSearchParams {
  constructor(init = '') {
    this._pairs = [];
    const input = typeof init === 'string' ? init.replace(/^\?/, '') : String(init || '');
    if (!input) return;
    const parts = input.split('&');
    for (const part of parts) {
      if (!part) continue;
      const eq = part.indexOf('=');
      const k = decodeURIComponent(eq >= 0 ? part.slice(0, eq) : part);
      const v = decodeURIComponent(eq >= 0 ? part.slice(eq + 1) : '');
      this._pairs.push([k, v]);
    }
  }

  set(name, value) {
    const key = String(name);
    const val = String(value);
    let found = false;
    const next = [];
    for (const [k, v] of this._pairs) {
      if (k === key) {
        if (!found) next.push([k, val]);
        found = true;
      } else {
        next.push([k, v]);
      }
    }
    if (!found) next.push([key, val]);
    this._pairs = next;
  }

  append(name, value) {
    this._pairs.push([String(name), String(value)]);
  }

  entries() {
    return this._pairs[Symbol.iterator]();
  }

  [Symbol.iterator]() {
    return this.entries();
  }

  toString() {
    return this._pairs.map(([k, v]) => `${encodeURIComponent(k)}=${encodeURIComponent(v)}`).join('&');
  }
}

const URLSearchParamsImpl = typeof NativeURLSearchParams === 'function' ? NativeURLSearchParams : SimpleURLSearchParams;

function FallbackURL(input) {
  if (!(this instanceof FallbackURL)) return new FallbackURL(input);
  const href = String(input);
  const parsed = typeof bindingUrl.parse === 'function' ? bindingUrl.parse(href) : null;
  if (!parsed || typeof parsed !== 'object' || typeof parsed.protocol !== 'string' || parsed.protocol.length === 0) {
    const err = new TypeError('Invalid URL');
    err.code = 'ERR_INVALID_URL';
    throw err;
  }
  const protocol = String(parsed.protocol).toLowerCase();
  const remainder = href.slice((href.indexOf('://') + 3) >>> 0);
  const isFile = protocol === 'file:';
  if (!isFile && (remainder.length === 0 || remainder[0] === ':' || remainder[0] === '/')) {
    const err = new TypeError('Invalid URL');
    err.code = 'ERR_INVALID_URL';
    throw err;
  }
  Object.defineProperty(this, 'href', { __proto__: null, value: href, enumerable: false, configurable: true, writable: true });
  Object.defineProperty(this, 'protocol', { __proto__: null, value: protocol, enumerable: false, configurable: true, writable: true });
  Object.defineProperty(this, 'hostname', { __proto__: null, value: parsed.hostname || '', enumerable: false, configurable: true, writable: true });
  Object.defineProperty(this, 'pathname', { __proto__: null, value: parsed.pathname || '/', enumerable: false, configurable: true, writable: true });
  Object.defineProperty(this, 'search', { __proto__: null, value: parsed.search || '', enumerable: false, configurable: true, writable: true });
  Object.defineProperty(this, 'hash', { __proto__: null, value: parsed.hash || '', enumerable: false, configurable: true, writable: true });
  Object.defineProperty(this, 'username', { __proto__: null, value: parsed.username || '', enumerable: false, configurable: true, writable: true });
  Object.defineProperty(this, 'password', { __proto__: null, value: parsed.password || '', enumerable: false, configurable: true, writable: true });
  Object.defineProperty(this, 'port', { __proto__: null, value: parsed.port || '', enumerable: false, configurable: true, writable: true });
  Object.defineProperty(this, Symbol.toStringTag, { __proto__: null, value: 'URL', enumerable: false, configurable: true });
}

const URLImpl = typeof NativeURL === 'function' ? NativeURL : FallbackURL;

function domainToASCII(domain) {
  return typeof bindingUrl.domainToASCII === 'function' ?
    bindingUrl.domainToASCII(`${domain}`) :
    String(domain || '').toLowerCase();
}

function domainToUnicode(domain) {
  return typeof bindingUrl.domainToUnicode === 'function' ?
    bindingUrl.domainToUnicode(`${domain}`) :
    String(domain || '');
}

function isURL(self) {
  if (typeof URLImpl === 'function' && self instanceof URLImpl) return true;
  return self != null &&
    typeof self === 'object' &&
    self[Symbol.toStringTag] === 'URL';
}

function getPathFromURLPosix(url) {
  if (url.hostname !== '') {
    throw new ERR_INVALID_FILE_URL_HOST(process.platform);
  }
  const pathname = url.pathname;
  for (let n = 0; n < pathname.length; n++) {
    if (pathname[n] === '%') {
      const third = pathname.codePointAt(n + 2) | 0x20;
      if (pathname[n + 1] === '2' && third === 102) {
        throw new ERR_INVALID_FILE_URL_PATH('must not include encoded / characters', url.input || url);
      }
    }
  }
  return pathname.includes('%') ? decodeURIComponent(pathname) : pathname;
}

function getPathFromURLWin32(url) {
  const hostname = url.hostname;
  let pathname = url.pathname;
  for (let n = 0; n < pathname.length; n++) {
    if (pathname[n] === '%') {
      const third = pathname.codePointAt(n + 2) | 0x20;
      if ((pathname[n + 1] === '2' && third === 102) || (pathname[n + 1] === '5' && third === 99)) {
        throw new ERR_INVALID_FILE_URL_PATH('must not include encoded \\ or / characters', url.input || url);
      }
    }
  }
  pathname = pathname.replace(FORWARD_SLASH, '\\');
  if (pathname.includes('%')) pathname = decodeURIComponent(pathname);

  if (hostname !== '') {
    return `\\\\${domainToUnicode(hostname)}${pathname}`;
  }
  const letter = pathname.codePointAt(1) | 0x20;
  const sep = pathname.charAt(2);
  if (letter < 97 || letter > 122 || sep !== ':') {
    throw new ERR_INVALID_FILE_URL_PATH('must be absolute', url.input || url);
  }
  return pathname.slice(1);
}

function fileURLToPath(input, options = undefined) {
  const windows = options?.windows;
  let href = input;
  if (typeof input === 'string') {
    href = input;
  } else if (input && typeof input === 'object' && typeof input.href === 'string') {
    href = input.href;
  } else {
    throw new ERR_INVALID_ARG_TYPE('path', ['string', 'URL'], input);
  }
  const parsed = typeof bindingUrl.parse === 'function' ? bindingUrl.parse(String(href)) : {};
  const protocol = parsed.protocol;
  if (!String(protocol || '').startsWith('file')) throw new ERR_INVALID_URL_SCHEME('file');
  let inputUrl;
  const GlobalURL = globalThis.URL;
  if (typeof GlobalURL === 'function') {
    try {
      inputUrl = new GlobalURL(String(href));
    } catch {}
  }
  if (!inputUrl) {
    try {
      inputUrl = new URLImpl(String(href));
    } catch {
      inputUrl = href;
    }
  }
  const url = {
    hostname: parsed.hostname || '',
    pathname: parsed.pathname || '/',
    input: inputUrl,
  };
  return (windows ?? isWindows) ? getPathFromURLWin32(url) : getPathFromURLPosix(url);
}

function fileURLToPathBuffer(input, options = undefined) {
  return Buffer.from(fileURLToPath(input, options), 'utf8');
}

function pathToFileURL(filepath, options = undefined) {
  if (typeof filepath !== 'string') {
    throw new ERR_INVALID_ARG_TYPE('path', 'string', filepath);
  }
  const windows = options?.windows ?? isWindows;
  const isUNC = windows && filepath.startsWith('\\\\');
  let resolved = isUNC ? filepath : (windows ? path.win32.resolve(filepath) : path.posix.resolve(filepath));
  if (isUNC || (windows && resolved.startsWith('\\\\'))) {
    const isExtendedUNC = resolved.startsWith('\\\\?\\UNC\\');
    const prefixLength = isExtendedUNC ? 8 : 2;
    const hostnameEndIndex = resolved.indexOf('\\', prefixLength);
    if (hostnameEndIndex === -1) {
      throw new ERR_INVALID_ARG_VALUE('path', resolved);
    }
    if (hostnameEndIndex === 2) {
      throw new ERR_INVALID_ARG_VALUE('path', resolved);
    }
    const hostname = resolved.slice(prefixLength, hostnameEndIndex);
    const href = typeof bindingUrl.pathToFileURL === 'function' ?
      bindingUrl.pathToFileURL(resolved.slice(hostnameEndIndex), windows, hostname) :
      `file://${hostname}${resolved.slice(hostnameEndIndex).replace(/\\/g, '/')}`;
    return new URLImpl(href);
  }
  const last = filepath.charCodeAt(filepath.length - 1);
  if ((last === 47 || (windows && last === 92)) && resolved[resolved.length - 1] !== path.sep) resolved += '/';
  const href = typeof bindingUrl.pathToFileURL === 'function' ?
    bindingUrl.pathToFileURL(resolved, windows) :
    `file://${resolved.replace(/\\/g, '/')}`;
  return new URLImpl(href);
}

function urlToHttpOptions(urlObj) {
  if (urlObj === null || typeof urlObj !== 'object') {
    throw new ERR_INVALID_ARG_TYPE('url', 'object', urlObj);
  }
  const isRealURL = isURL(urlObj);
  let source = urlObj;
  if (isRealURL &&
      (source.protocol === undefined || source.hostname === undefined) &&
      typeof source.href === 'string') {
    if (typeof bindingUrl.parse === 'function') {
      const parsed = bindingUrl.parse(source.href);
      if (parsed && typeof parsed === 'object' && parsed.protocol !== undefined) {
        source = parsed;
      }
    }
  }
  const {
    hostname,
    pathname,
    port,
    username,
    password,
    search,
  } = source;
  const maybe = (v) => (!isRealURL && v === '' ? undefined : v);
  const protocol = maybe(source.protocol);
  const hash = maybe(source.hash);
  const searchValue = maybe(search);
  const pathnameValue = maybe(pathname);
  const hrefValue = isRealURL ? source.href : undefined;
  return {
    __proto__: null,
    ...urlObj,
    protocol,
    hostname: maybe(hostname && hostname[0] === '[' ? hostname.slice(1, -1) : hostname),
    hash,
    search: searchValue,
    pathname: pathnameValue,
    path: `${pathnameValue || ''}${searchValue || ''}`,
    href: hrefValue,
    port: port !== '' ? Number(port) : undefined,
    auth: username || password ? `${decodeURIComponent(username)}:${decodeURIComponent(password)}` : undefined,
  };
}

module.exports = {
  URL: URLImpl,
  URLSearchParams: URLSearchParamsImpl,
  domainToASCII,
  domainToUnicode,
  fileURLToPath,
  fileURLToPathBuffer,
  pathToFileURL,
  isURL,
  urlToHttpOptions,
  unsafeProtocol,
  hostlessProtocol,
  slashedProtocol,
};
