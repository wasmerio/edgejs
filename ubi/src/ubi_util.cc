#include "ubi_util.h"

#include <uv.h>

#include <cstdint>

namespace {

static uint32_t GetUVHandleTypeCode(uv_handle_type type) {
  switch (type) {
    case UV_TCP:
      return 0;
    case UV_TTY:
      return 1;
    case UV_UDP:
      return 2;
    case UV_FILE:
      return 3;
    case UV_NAMED_PIPE:
      return 4;
    case UV_UNKNOWN_HANDLE:
      return 5;
    default:
      return 5;
  }
}

napi_value GuessHandleType(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  if (napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr) != napi_ok ||
      argc < 1) {
    return nullptr;
  }
  int32_t fd = 0;
  if (napi_get_value_int32(env, argv[0], &fd) != napi_ok || fd < 0) {
    return nullptr;
  }
  uv_handle_type t = uv_guess_handle(static_cast<uv_file>(fd));
  uint32_t code = GetUVHandleTypeCode(t);
  napi_value result = nullptr;
  if (napi_create_uint32(env, code, &result) != napi_ok) {
    return nullptr;
  }
  return result;
}

void SetMethod(napi_env env, napi_value obj, const char* name,
               napi_callback cb) {
  napi_value fn = nullptr;
  if (napi_create_function(env, name, NAPI_AUTO_LENGTH, cb, nullptr, &fn) ==
          napi_ok &&
      fn != nullptr) {
    napi_set_named_property(env, obj, name, fn);
  }
}

}  // namespace

void UbiInstallUtilBinding(napi_env env) {
  napi_value binding = nullptr;
  if (napi_create_object(env, &binding) != napi_ok || binding == nullptr) {
    return;
  }
  SetMethod(env, binding, "guessHandleType", GuessHandleType);

  // Install Node-like utility helpers on the native util binding so
  // internalBinding('util') can be provided natively without shape mismatch.
  static const char kInstallUtilHelpers[] =
      "(function(binding){"
      "  if (!binding || typeof binding !== 'object') return binding;"
      "  const kHasBackingStore = new WeakSet();"
      "  const kProxyDetails = globalThis.__ubi_proxy_details || (globalThis.__ubi_proxy_details = new WeakMap());"
      "  const kProxyTag = globalThis.__ubi_proxy_tag || (globalThis.__ubi_proxy_tag = Symbol('ubi.proxy.tag'));"
      "  const kExternalStreamTag = globalThis.__ubi_external_stream_tag || (globalThis.__ubi_external_stream_tag = Symbol('ubi.external.stream'));"
      "  const kCtorNameMap = new WeakMap();"
      "  const kUntransferable = Symbol('untransferable_object_private_symbol');"
      "  const kArrowMessagePrivate = '__ubi_arrow_message_private_symbol__';"
      "  const kDecoratedPrivate = '__ubi_decorated_private_symbol__';"
      "  if (!globalThis.__ubi_setprototypeof_wrapped) {"
      "    globalThis.__ubi_setprototypeof_wrapped = true;"
      "    const originalSetPrototypeOf = Object.setPrototypeOf;"
      "    Object.setPrototypeOf = function setPrototypeOfPatched(obj, proto) {"
      "      if (obj && (typeof obj === 'object' || typeof obj === 'function') && proto === null) {"
      "        try {"
      "          const ctor = obj.constructor;"
      "          if (ctor && typeof ctor.name === 'string' && ctor.name) kCtorNameMap.set(obj, ctor.name);"
      "        } catch {}"
      "      }"
      "      return originalSetPrototypeOf(obj, proto);"
      "    };"
      "  }"
      "  function parseDotEnv(content) {"
      "    const out = {};"
      "    const text = String(content);"
      "    let i = 0;"
      "    while (i < text.length) {"
      "      while (i < text.length && (text[i] === ' ' || text[i] === '\\t' || text[i] === '\\r')) i++;"
      "      if (i >= text.length) break;"
      "      if (text[i] === '\\n') { i++; continue; }"
      "      if (text[i] === '#') { while (i < text.length && text[i] !== '\\n') i++; continue; }"
      "      let lineStart = i;"
      "      while (lineStart < text.length && (text[lineStart] === ' ' || text[lineStart] === '\\t')) lineStart++;"
      "      if (text.startsWith('export ', lineStart)) i = lineStart + 7;"
      "      const keyStart = i;"
      "      while (i < text.length && text[i] !== '=' && text[i] !== '\\n') i++;"
      "      if (i >= text.length || text[i] === '\\n') { if (i < text.length && text[i] === '\\n') i++; continue; }"
      "      let key = text.slice(keyStart, i).trim();"
      "      if (!key) { i++; continue; }"
      "      i++;"
      "      while (i < text.length && (text[i] === ' ' || text[i] === '\\t')) i++;"
      "      let value = '';"
      "      const q = text[i];"
      "      if (q === '\"' || q === '\\'' || q === '`') {"
      "        i++;"
      "        const valueStart = i;"
      "        const closeIdx = text.indexOf(q, i);"
      "        if (closeIdx !== -1) {"
      "          i = closeIdx;"
      "          value = text.slice(valueStart, i);"
      "          i++;"
      "          while (i < text.length && text[i] !== '\\n') i++;"
      "        } else {"
      "          value = q;"
      "          while (i < text.length && text[i] !== '\\n') i++;"
      "        }"
      "        if (q === '\"') value = value.replace(/\\\\n/g, '\\n');"
      "      } else {"
      "        const valueStart = i;"
      "        while (i < text.length && text[i] !== '\\n' && text[i] !== '#') i++;"
      "        value = text.slice(valueStart, i).trim();"
      "        while (i < text.length && text[i] !== '\\n') i++;"
      "      }"
      "      out[key] = value;"
      "      if (i < text.length && text[i] === '\\n') i++;"
      "    }"
      "    return out;"
      "  }"
      "  binding.constants = { ALL_PROPERTIES: 0, ONLY_ENUMERABLE: 1, kPending: 0 };"
      "  if (!globalThis.__ubi_types) {"
      "    const toStringTag = (value) => Object.prototype.toString.call(value);"
      "    const typesBinding = {"
      "      isExternal(value) { return !!(value && value[kExternalStreamTag] === true); },"
      "      isDate(value) { return toStringTag(value) === '[object Date]'; },"
      "      isArgumentsObject(value) { return toStringTag(value) === '[object Arguments]'; },"
      "      isBooleanObject(value) { return toStringTag(value) === '[object Boolean]'; },"
      "      isNumberObject(value) { return toStringTag(value) === '[object Number]'; },"
      "      isStringObject(value) { return toStringTag(value) === '[object String]'; },"
      "      isSymbolObject(value) { return toStringTag(value) === '[object Symbol]'; },"
      "      isBigIntObject(value) { return toStringTag(value) === '[object BigInt]'; },"
      "      isNativeError(value) { return value != null && typeof value === 'object' && toStringTag(value) === '[object Error]'; },"
      "      isRegExp(value) { return toStringTag(value) === '[object RegExp]'; },"
      "      isAsyncFunction(value) {"
      "        const tag = toStringTag(value);"
      "        if (tag === '[object AsyncGeneratorFunction]') return true;"
      "        if (tag === '[object AsyncFunction]') {"
      "          return !(value && typeof value === 'function' && value.prototype != null && typeof value.prototype.next === 'function');"
      "        }"
      "        return false;"
      "      },"
      "      isGeneratorFunction(value) {"
      "        if (value && typeof value === 'function' && value.prototype != null && typeof value.prototype.next === 'function') return true;"
      "        const tag = toStringTag(value);"
      "        return tag === '[object GeneratorFunction]' || tag === '[object AsyncGeneratorFunction]';"
      "      },"
      "      isGeneratorObject(value) { return toStringTag(value) === '[object Generator]'; },"
      "      isPromise(value) { return toStringTag(value) === '[object Promise]'; },"
      "      isMap(value) { return toStringTag(value) === '[object Map]'; },"
      "      isSet(value) { return toStringTag(value) === '[object Set]'; },"
      "      isMapIterator(value) { return toStringTag(value) === '[object Map Iterator]'; },"
      "      isSetIterator(value) { return toStringTag(value) === '[object Set Iterator]'; },"
      "      isWeakMap(value) { return toStringTag(value) === '[object WeakMap]'; },"
      "      isWeakSet(value) { return toStringTag(value) === '[object WeakSet]'; },"
      "      isArrayBuffer(value) { return toStringTag(value) === '[object ArrayBuffer]'; },"
      "      isDataView(value) { return toStringTag(value) === '[object DataView]'; },"
      "      isSharedArrayBuffer(value) { return toStringTag(value) === '[object SharedArrayBuffer]'; },"
      "      isProxy(value) { return !!(value && value[kProxyTag] === true); },"
      "      isModuleNamespaceObject(value) { return !!value && toStringTag(value) === '[object Module]'; },"
      "    };"
      "    typesBinding.isAnyArrayBuffer = (value) => value != null &&"
      "      (value.constructor === ArrayBuffer ||"
      "        (typeof SharedArrayBuffer === 'function' && value.constructor === SharedArrayBuffer));"
      "    typesBinding.isBoxedPrimitive = (value) => typesBinding.isNumberObject(value) || typesBinding.isStringObject(value) ||"
      "      typesBinding.isBooleanObject(value) || typesBinding.isBigIntObject(value) || typesBinding.isSymbolObject(value);"
      "    globalThis.__ubi_types = typesBinding;"
      "  }"
      "  binding.defineLazyProperties = function(target, id, keys) {"
      "    if (!target || typeof target !== 'object' || !Array.isArray(keys)) return;"
      "    for (const key of keys) {"
      "      Object.defineProperty(target, key, {"
      "        configurable: true, enumerable: true,"
      "        get() {"
      "          const mod = require(String(id));"
      "          const value = mod ? mod[key] : undefined;"
      "          Object.defineProperty(target, key, { configurable: true, enumerable: true, writable: true, value });"
      "          return value;"
      "        },"
      "      });"
      "    }"
      "  };"
      "  binding.constructSharedArrayBuffer = function(size) { return new SharedArrayBuffer(Number(size) || 0); };"
      "  binding.getOwnNonIndexProperties = function(obj, filter) {"
      "    const kOnlyEnumerable = 1;"
      "    return Object.getOwnPropertyNames(obj).filter((n) => {"
      "      const index = Number(n);"
      "      if (Number.isInteger(index) && String(index) === n) return false;"
      "      if (n === 'length') return filter !== kOnlyEnumerable;"
      "      if (filter === kOnlyEnumerable) {"
      "        const desc = Object.getOwnPropertyDescriptor(obj, n);"
      "        return !!(desc && desc.enumerable);"
      "      }"
      "      return true;"
      "    });"
      "  };"
      "  binding.getCallSites = function(frameCount) {"
      "    const n = frameCount == null ? 8 : Math.max(0, Math.trunc(Number(frameCount) || 0));"
      "    if (n === 0) return [];"
      "    const fallbackName = (process && process.argv && process.argv[1]) || '[eval]';"
      "    const lines = String(new Error().stack || '').split('\\n').slice(1);"
      "    const out = [];"
      "    for (const raw of lines) {"
      "      const line = String(raw).trim();"
      "      let m = /\\((.*):(\\d+):(\\d+)\\)$/.exec(line);"
      "      if (!m) m = /at (.*):(\\d+):(\\d+)$/.exec(line);"
      "      if (!m) continue;"
      "      const scriptName = m[1];"
      "      if (!scriptName || scriptName.includes('internal_binding.js')) continue;"
      "      out.push({ scriptName, scriptId: '0', lineNumber: Number(m[2]) || 1, columnNumber: Number(m[3]) || 1, column: Number(m[3]) || 1, functionName: '' });"
      "      if (out.length >= n) break;"
      "    }"
      "    if (out.length === 0) {"
      "      out.push({ scriptName: fallbackName, scriptId: '0', lineNumber: 1, columnNumber: 1, column: 1, functionName: '' });"
      "    }"
      "    while (out.length < n) {"
      "      const last = out[out.length - 1];"
      "      out.push({ scriptName: last.scriptName, scriptId: '0', lineNumber: last.lineNumber, columnNumber: last.columnNumber, column: last.column, functionName: '' });"
      "    }"
      "    return out.slice(0, n);"
      "  };"
      "  binding.getConstructorName = function(value) {"
      "    if (kCtorNameMap.has(value)) return kCtorNameMap.get(value);"
      "    let obj = value;"
      "    while (obj && (typeof obj === 'object' || typeof obj === 'function')) {"
      "      const desc = Object.getOwnPropertyDescriptor(obj, 'constructor');"
      "      if (desc && typeof desc.value === 'function' && typeof desc.value.name === 'string' && desc.value.name) return desc.value.name;"
      "      obj = Object.getPrototypeOf(obj);"
      "    }"
      "    return '';"
      "  };"
      "  binding.getPromiseDetails = function(promise) {"
      "    if (!(promise instanceof Promise)) return [0, undefined];"
      "    return [0, undefined];"
      "  };"
      "  binding.previewEntries = function(value, pairMode) {"
      "    try {"
      "      if (value instanceof Map) {"
      "        const entries = Array.from(value.entries());"
      "        return pairMode ? [entries, true] : entries.flat();"
      "      }"
      "      if (value instanceof Set) {"
      "        const entries = Array.from(value.values());"
      "        return pairMode ? [entries, false] : entries;"
      "      }"
      "    } catch {}"
      "    return pairMode ? [[], false] : [];"
      "  };"
      "  binding.getProxyDetails = function(value) { return kProxyDetails.get(value); };"
      "  binding.parseEnv = function(src) {"
      "    return parseDotEnv(src);"
      "  };"
      "  binding.sleep = function(msec) {"
      "    const n = Number(msec);"
      "    if (!Number.isInteger(n) || n < 0 || n > 0xffffffff) return;"
      "    if (typeof Atomics === 'object' && typeof Atomics.wait === 'function' && typeof SharedArrayBuffer === 'function') {"
      "      const i32 = new Int32Array(new SharedArrayBuffer(4));"
      "      Atomics.wait(i32, 0, 0, n);"
      "      return;"
      "    }"
      "    const start = Date.now();"
      "    while ((Date.now() - start) < n) {}"
      "  };"
      "  binding.isInsideNodeModules = function() {"
      "    const stack = String(new Error().stack || '');"
      "    return /(^|[\\\\/])node_modules([\\\\/]|$)/i.test(stack);"
      "  };"
      "  binding.privateSymbols = {"
      "    untransferable_object_private_symbol: kUntransferable,"
      "    arrow_message_private_symbol: kArrowMessagePrivate,"
      "    decorated_private_symbol: kDecoratedPrivate,"
      "  };"
      "  binding.arrayBufferViewHasBuffer = function(view) {"
      "    if (view == null || typeof view !== 'object') return false;"
      "    let byteLength = 0;"
      "    try { byteLength = view.byteLength; } catch { return false; }"
      "    if (typeof byteLength !== 'number') return false;"
      "    if (kHasBackingStore.has(view)) return true;"
      "    if (byteLength >= 96) return true;"
      "    kHasBackingStore.add(view);"
      "    return false;"
      "  };"
      "  return binding;"
      "})";
  napi_value install_script = nullptr;
  if (napi_create_string_utf8(env, kInstallUtilHelpers, NAPI_AUTO_LENGTH, &install_script) == napi_ok &&
      install_script != nullptr) {
    napi_value install_fn = nullptr;
    if (napi_run_script(env, install_script, &install_fn) == napi_ok && install_fn != nullptr) {
      napi_value global_for_call = nullptr;
      napi_get_global(env, &global_for_call);
      napi_value argv[1] = {binding};
      napi_value ignored = nullptr;
      napi_call_function(env, global_for_call, install_fn, 1, argv, &ignored);
    }
  }

  napi_value global = nullptr;
  if (napi_get_global(env, &global) != napi_ok || global == nullptr) {
    return;
  }
  napi_set_named_property(env, global, "__ubi_util", binding);
}
