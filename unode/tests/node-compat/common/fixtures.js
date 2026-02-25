'use strict';

module.exports = {
  path: function path() {
    const parts = [];
    for (let i = 0; i < arguments.length; i += 1) {
      parts.push(arguments[i]);
    }
    return __dirname + '/../fixtures/' + parts.join('/');
  },
};
