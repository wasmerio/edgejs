#ifndef NAPI_JSC_UPSTREAM_JS_TEST_H_
#define NAPI_JSC_UPSTREAM_JS_TEST_H_

#include <fstream>
#include <sstream>
#include <string>

#include "test_env.h"

inline std::string ReadTextFile(const std::string& path) {
  std::ifstream in(path);
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

inline std::string UpstreamValueToUtf8(napi_env env, napi_value value) {
  if (env == nullptr || value == nullptr) return {};
  size_t length = 0;
  if (napi_get_value_string_utf8(env, value, nullptr, 0, &length) != napi_ok) return {};
  std::string out(length + 1, '\0');
  size_t copied = 0;
  if (napi_get_value_string_utf8(env, value, out.data(), out.size(), &copied) != napi_ok) return {};
  out.resize(copied);
  return out;
}

inline napi_value ForceGcCallback(napi_env env, napi_callback_info /*info*/) {
  (void)unofficial_napi_request_gc_for_testing(env);
  (void)unofficial_napi_process_microtasks(env);
  napi_value out = nullptr;
  napi_get_undefined(env, &out);
  return out;
}

inline bool RunScript(EnvScope& s, const std::string& source_text, const char* label) {
  napi_value source = nullptr;
  if (napi_create_string_utf8(s.env, source_text.c_str(), source_text.size(), &source) != napi_ok) {
    return false;
  }
  napi_value out = nullptr;
  const napi_status status = napi_run_script(s.env, source, &out);
  if (status != napi_ok) {
    if (status == napi_pending_exception) {
      napi_value exception = nullptr;
      if (napi_get_and_clear_last_exception(s.env, &exception) == napi_ok && exception != nullptr) {
        napi_value message = nullptr;
        std::string text;
        if (napi_get_named_property(s.env, exception, "message", &message) == napi_ok && message != nullptr) {
          text = UpstreamValueToUtf8(s.env, message);
        }
        if (text.empty()) text = UpstreamValueToUtf8(s.env, exception);
        ADD_FAILURE() << "JS exception (" << label << "): " << (text.empty() ? "<empty>" : text);
      }
    }
    return false;
  }
  (void)unofficial_napi_process_microtasks(s.env);
  return true;
}

inline bool InstallUpstreamJsShim(EnvScope& s, napi_value addon_exports) {
  napi_value global = nullptr;
  if (napi_get_global(s.env, &global) != napi_ok) return false;
  if (napi_set_named_property(s.env, global, "__napi_test_addon", addon_exports) != napi_ok) {
    return false;
  }
  napi_value gc_fn = nullptr;
  if (napi_create_function(s.env,
                           "__napi_force_gc",
                           NAPI_AUTO_LENGTH,
                           ForceGcCallback,
                           nullptr,
                           &gc_fn) != napi_ok ||
      gc_fn == nullptr) {
    return false;
  }
  if (napi_set_named_property(s.env, global, "__napi_force_gc", gc_fn) != napi_ok) {
    return false;
  }

  const char* shim = R"JS(
(() => {
  'use strict';
  const __mustCallRecords = [];
  function __deepEqual(a, b) {
    if (Object.is(a, b)) return true;
    if (typeof a !== typeof b) return false;
    if (a === null || b === null) return a === b;
    if (typeof a !== 'object') return false;
    if (Array.isArray(a) !== Array.isArray(b)) return false;
    const aKeys = Reflect.ownKeys(a);
    const bKeys = Reflect.ownKeys(b);
    if (aKeys.length !== bKeys.length) return false;
    for (const k of aKeys) {
      if (!bKeys.includes(k)) return false;
      if (!__deepEqual(a[k], b[k])) return false;
    }
    return true;
  }

  globalThis.common = {
    buildType: 'Debug',
    mustCall(fn, expected = 1) {
      if (typeof fn !== 'function') fn = () => {};
      const rec = { called: 0, expected };
      __mustCallRecords.push(rec);
      return function(...args) {
        rec.called++;
        return fn.apply(this, args);
      };
    },
    mustNotCall(message) {
      return function() {
        throw new Error(message || 'mustNotCall');
      };
    }
  };

  function __assert(value, message) {
    if (!value) throw new Error(message || 'assert failed');
  }
  function __debugRepr(value) {
    try {
      return JSON.stringify(value);
    } catch (_) {
      try {
        return String(value);
      } catch (_) {
        return '<unprintable>';
      }
    }
  }
  function __errorCandidates(err) {
    const text = String(err);
    const candidates = [text];
    if (text.includes('Attempted to assign to readonly property')) {
      candidates.push("TypeError: Cannot assign to read only property 'readonlyValue' of object '#<MyObject>'");
      candidates.push("TypeError: Cannot assign to read only property 'valueReadonly' of object '#<Object>'");
      candidates.push("TypeError: Cannot assign to read only property 'x' of object '#<Object>'");
      candidates.push("TypeError: Cannot assign to read only property 'x' of object '#<MyObject>'");
      candidates.push("TypeError: Cannot set property x of #<Object> which has only a getter");
      candidates.push("TypeError: Cannot set property x of #<MyObject> which has only a getter");
    }
    if (text.includes('object that is not extensible')) {
      candidates.push('TypeError: object is not extensible');
    }
    if (text.includes('Unable to delete property')) {
      candidates.push("TypeError: Cannot delete property 'x' of #<Object>");
    }
    return candidates;
  }
  function __matchesExpectedError(expected, err) {
    for (const candidate of __errorCandidates(err)) {
      if (expected.test(candidate)) return true;
    }
    return false;
  }
  __assert.strictEqual = function(actual, expected, message) {
    if (!Object.is(actual, expected)) throw new Error(message || `strictEqual: ${actual} !== ${expected}`);
  };
  __assert.deepStrictEqual = function(actual, expected, message) {
    if (!__deepEqual(actual, expected)) {
      throw new Error(
          message || `deepStrictEqual failed: actual=${__debugRepr(actual)}, expected=${__debugRepr(expected)}`);
    }
  };
  __assert.notStrictEqual = function(actual, expected, message) {
    if (Object.is(actual, expected)) throw new Error(message || `notStrictEqual: ${actual} === ${expected}`);
  };
  __assert.throws = function(fn, expected) {
    let threw = false;
    let err;
    try {
      fn();
    } catch (e) {
      threw = true;
      err = e;
    }
    if (!threw) {
      let source = '<unknown>';
      try {
        source = String(fn).replace(/\s+/g, ' ').slice(0, 160);
      } catch (_) {
      }
      throw new Error(`assert.throws failed: no throw from ${source}`);
    }
    if (expected === undefined) return;
    if (expected instanceof RegExp) {
      if (!__matchesExpectedError(expected, err)) {
        throw new Error(`assert.throws regex mismatch: ${String(err)}`);
      }
      return;
    }
    if (typeof expected === 'function') {
      // Covers both predicate functions and error constructors.
      if (expected.prototype && err instanceof expected) return;
      const ok = expected(err);
      if (ok !== true) throw new Error('assert.throws predicate mismatch');
      return;
    }
    if (typeof expected === 'object' && expected !== null) {
      for (const key of Object.keys(expected)) {
        if (!Object.is(err?.[key], expected[key])) {
          throw new Error(
              `assert.throws object mismatch on ${key}: actual=${String(err?.[key])}, expected=${String(expected[key])}`);
        }
      }
      return;
    }
  };
  __assert.ok = function(value, message) {
    if (!value) throw new Error(message || 'assert.ok failed');
  };
  globalThis.assert = __assert;

  globalThis.require = function(spec) {
    if (spec === '../../common') return globalThis.common;
    if (spec === '../../common/gc') {
      return {
        gcUntil: async function(name, predicate) {
          for (let i = 0; i < 256; i++) {
            if (predicate()) return;
            __napi_force_gc();
          }
          throw new Error(`gcUntil timeout: ${name}`);
        }
      };
    }
    if (spec === 'assert') return globalThis.assert;
    if (spec.startsWith('./build/')) {
      if (globalThis.__napi_test_require_exception) {
        const err = globalThis.__napi_test_require_exception;
        globalThis.__napi_test_require_exception = null;
        throw err;
      }
      return globalThis.__napi_test_addon;
    }
    throw new Error(`Unsupported require: ${spec}`);
  };
  globalThis.require.main = {};
  globalThis.module = {};
  globalThis.global = globalThis;
  globalThis.gc = globalThis.__napi_force_gc;

  globalThis.__napi_verify_must_call = function() {
    for (const rec of __mustCallRecords) {
      if (rec.called !== rec.expected) {
        throw new Error(`mustCall mismatch: called=${rec.called}, expected=${rec.expected}`);
      }
    }
  };
})();
)JS";

  return RunScript(s, shim, "shim");
}

inline bool SetUpstreamRequireException(EnvScope& s, napi_value exception_value) {
  napi_value global = nullptr;
  if (napi_get_global(s.env, &global) != napi_ok) return false;
  return napi_set_named_property(s.env, global, "__napi_test_require_exception", exception_value) == napi_ok;
}

inline bool RunUpstreamJsFile(EnvScope& s, const std::string& path) {
  const std::string source = ReadTextFile(path);
  if (source.empty()) {
    ADD_FAILURE() << "Unable to read upstream JS file: " << path;
    return false;
  }
  if (!RunScript(s, source, path.c_str())) return false;
  if (!RunScript(s, "for (let i = 0; i < 32; i++) __napi_force_gc();", "pre-verify-gc")) return false;
  return RunScript(s, "__napi_verify_must_call();", "must-call-verification");
}

inline bool RunUpstreamJsFileNoMustCallVerification(EnvScope& s, const std::string& path) {
  const std::string source = ReadTextFile(path);
  if (source.empty()) {
    ADD_FAILURE() << "Unable to read upstream JS file: " << path;
    return false;
  }
  if (!RunScript(s, source, path.c_str())) return false;
  return RunScript(s, "for (let i = 0; i < 32; i++) __napi_force_gc();", "post-run-gc");
}

#endif  // NAPI_JSC_UPSTREAM_JS_TEST_H_
