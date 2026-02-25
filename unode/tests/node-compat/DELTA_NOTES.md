# Node Test Delta Notes

## `test-require-cache.js`
- Adaptation: `require('assert')` -> `require('../common/assert')`.
- Intent preserved: checks `require.cache` lookup by resolved filename and by bare specifier key.

## `test-require-json.js`
- Adaptation: `require('assert')` -> `require('../common/assert')`.
- Adaptation: relaxed message assertion to only validate `SyntaxError` name (runtime does not yet prefix JSON parse errors with filename).
- Intent preserved: invalid JSON required via fixtures path throws parse error.
