'use strict';

const EventEmitter = require('events');

function Stream() {
  EventEmitter.call(this);
  this.writable = true;
}
Object.setPrototypeOf(Stream.prototype, EventEmitter.prototype);
Object.setPrototypeOf(Stream, EventEmitter);

Stream.prototype.write = function write(_chunk, cb) {
  if (typeof cb === 'function') cb(null);
  return true;
};

Stream.prototype.pipe = function pipe(dest) {
  if (dest && typeof dest.emit === 'function') {
    this.on('end', function onEnd() {
      if (typeof dest.end === 'function') dest.end();
    });
  }
  return dest;
};

function Writable(options = {}) {
  Stream.call(this);
  this._writeImpl = typeof options.write === 'function' ?
    options.write :
    (_chunk, _encoding, callback) => callback();
}
Object.setPrototypeOf(Writable.prototype, Stream.prototype);
Object.setPrototypeOf(Writable, Stream);

Writable.prototype.write = function write(chunk, encoding, cb) {
  if (typeof encoding === 'function') {
    cb = encoding;
    encoding = 'utf8';
  }
  this._writeImpl(chunk, encoding || 'utf8', (err) => {
    if (!err) this.emit('drain');
    if (typeof cb === 'function') cb(err || null);
  });
  return true;
};

Writable.prototype.end = function end(chunk, encoding, cb) {
  if (chunk !== undefined) this.write(chunk, encoding);
  this.emit('finish');
  if (typeof cb === 'function') cb();
  return this;
};

module.exports = Stream;
module.exports.Stream = Stream;
module.exports.Writable = Writable;
