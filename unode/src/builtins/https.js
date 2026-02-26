'use strict';

const http = require('http');
const { URL } = require('url');

class Agent extends http.Agent {
  constructor(options) {
    super(options);
    this.defaultPort = 443;
    this.protocol = 'https:';
  }
}

const globalAgent = new Agent();

function normalizeRequestArgs(input, options, cb) {
  let requestOptions = options;
  let callback = cb;

  if (typeof requestOptions === 'function') {
    callback = requestOptions;
    requestOptions = undefined;
  }

  if (typeof input === 'string' || input instanceof URL) {
    const url = input instanceof URL ? input : new URL(input);
    if (url.protocol !== 'https:') {
      const err = new TypeError(`Protocol "${url.protocol}" not supported. Expected "https:"`);
      err.code = 'ERR_INVALID_PROTOCOL';
      throw err;
    }
    requestOptions = { ...(requestOptions || {}), ...urlToRequestOptions(url) };
  } else if (input && typeof input === 'object') {
    requestOptions = { ...input, ...(requestOptions || {}) };
  } else {
    requestOptions = requestOptions || {};
  }

  if (!Object.prototype.hasOwnProperty.call(requestOptions, 'agent')) {
    requestOptions.agent = globalAgent;
  }
  requestOptions.protocol = 'https:';
  requestOptions.defaultPort = requestOptions.defaultPort || 443;
  requestOptions.port = requestOptions.port || requestOptions.defaultPort;
  if (requestOptions.agent === false) {
    // Allow explicit opt-out from pooling while still routing over plain sockets.
    requestOptions.protocol = 'http:';
  }

  return { requestOptions, callback };
}

function urlToRequestOptions(url) {
  return {
    protocol: 'https:',
    hostname: url.hostname && url.hostname.startsWith('[') ? url.hostname.slice(1, -1) : url.hostname,
    hash: url.hash,
    search: url.search,
    pathname: url.pathname,
    path: `${url.pathname || ''}${url.search || ''}`,
    href: url.href,
    port: url.port ? Number(url.port) : undefined,
    host: url.hostname ? `${url.hostname}${url.port ? `:${url.port}` : ''}` : undefined,
    auth: url.username || url.password ? `${decodeURIComponent(url.username)}:${decodeURIComponent(url.password)}` : undefined,
  };
}

function request(input, options, cb) {
  const { requestOptions, callback } = normalizeRequestArgs(input, options, cb);
  return http.request(requestOptions, callback);
}

function get(input, options, cb) {
  const req = request(input, options, cb);
  req.end();
  return req;
}

function createServer(options, listener) {
  if (typeof options === 'function') {
    listener = options;
  }
  return http.createServer((req, res) => {
    if (req && req.socket) req.socket._secureEstablished = true;
    if (typeof listener === 'function') listener(req, res);
  });
}

module.exports = {
  Agent,
  globalAgent,
  request,
  get,
  createServer,
  Server: http.Server,
};
