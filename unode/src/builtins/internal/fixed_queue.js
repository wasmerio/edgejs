'use strict';

class FixedQueue {
  constructor() {
    this._list = [];
  }

  push(value) {
    this._list.push(value);
  }

  shift() {
    return this._list.shift();
  }
}

module.exports = FixedQueue;
