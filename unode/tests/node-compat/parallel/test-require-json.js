'use strict';
require('../common');
const assert = require('../common/assert');
const fixtures = require('../common/fixtures');

assert.throws(function() {
  require(fixtures.path('invalid.json'));
}, {
  name: 'SyntaxError',
});
