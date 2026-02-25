#include "unode_runtime.h"

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <stdlib.h>
extern char** _environ;
#else
#include <unistd.h>
extern char** environ;
#endif

#include "unode_fs.h"
#include "unode_buffer.h"
#include "unode_encoding.h"
#include "unode_module_loader.h"
#include "unode_os.h"
#include "unode_string_decoder.h"
#include "unode_url.h"

namespace {

void CopyProcessEnvironmentToObject(napi_env env, napi_value env_obj) {
  char** env_iter =
#if defined(_WIN32)
      _environ;
#else
      environ;
#endif
  for (; env_iter != nullptr && *env_iter != nullptr; ++env_iter) {
    std::string entry(*env_iter);
    size_t eq = entry.find('=');
    if (eq == std::string::npos) continue;
    std::string key = entry.substr(0, eq);
    std::string val = entry.substr(eq + 1);
    napi_value v = nullptr;
    if (napi_create_string_utf8(env, val.c_str(), NAPI_AUTO_LENGTH, &v) == napi_ok && v != nullptr) {
      napi_set_named_property(env, env_obj, key.c_str(), v);
    }
  }
}

std::string ReadTextFile(const char* path) {
  if (path == nullptr || path[0] == '\0') {
    return "";
  }
  std::ifstream in(path);
  if (!in.is_open()) {
    return "";
  }
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

std::string StatusToString(napi_status status) {
  switch (status) {
    case napi_ok:
      return "napi_ok";
    case napi_invalid_arg:
      return "napi_invalid_arg";
    case napi_object_expected:
      return "napi_object_expected";
    case napi_string_expected:
      return "napi_string_expected";
    case napi_name_expected:
      return "napi_name_expected";
    case napi_function_expected:
      return "napi_function_expected";
    case napi_number_expected:
      return "napi_number_expected";
    case napi_boolean_expected:
      return "napi_boolean_expected";
    case napi_array_expected:
      return "napi_array_expected";
    case napi_generic_failure:
      return "napi_generic_failure";
    case napi_pending_exception:
      return "napi_pending_exception";
    case napi_cancelled:
      return "napi_cancelled";
    case napi_escape_called_twice:
      return "napi_escape_called_twice";
    case napi_handle_scope_mismatch:
      return "napi_handle_scope_mismatch";
    case napi_callback_scope_mismatch:
      return "napi_callback_scope_mismatch";
    case napi_queue_full:
      return "napi_queue_full";
    case napi_closing:
      return "napi_closing";
    case napi_bigint_expected:
      return "napi_bigint_expected";
    case napi_date_expected:
      return "napi_date_expected";
    case napi_arraybuffer_expected:
      return "napi_arraybuffer_expected";
    case napi_detachable_arraybuffer_expected:
      return "napi_detachable_arraybuffer_expected";
    case napi_would_deadlock:
      return "napi_would_deadlock";
    case napi_no_external_buffers_allowed:
      return "napi_no_external_buffers_allowed";
    case napi_cannot_run_js:
      return "napi_cannot_run_js";
    default:
      return "napi_unknown_error";
  }
}

std::string GetAndClearPendingException(napi_env env, bool* is_process_exit, int* process_exit_code) {
  if (is_process_exit != nullptr) *is_process_exit = false;
  if (process_exit_code != nullptr) *process_exit_code = 1;
  bool pending = false;
  if (napi_is_exception_pending(env, &pending) != napi_ok || !pending) {
    return "";
  }

  napi_value exception = nullptr;
  if (napi_get_and_clear_last_exception(env, &exception) != napi_ok || exception == nullptr) {
    return "";
  }

  bool has_exit_code = false;
  if (napi_has_named_property(env, exception, "__unodeExitCode", &has_exit_code) == napi_ok && has_exit_code) {
    napi_value exit_code_value = nullptr;
    int32_t code = 1;
    if (napi_get_named_property(env, exception, "__unodeExitCode", &exit_code_value) == napi_ok &&
        exit_code_value != nullptr &&
        napi_get_value_int32(env, exit_code_value, &code) == napi_ok) {
      if (is_process_exit != nullptr) *is_process_exit = true;
      if (process_exit_code != nullptr) *process_exit_code = code;
      return "";
    }
  }

  napi_value exception_string = nullptr;
  if (napi_coerce_to_string(env, exception, &exception_string) != napi_ok || exception_string == nullptr) {
    return "";
  }

  size_t length = 0;
  if (napi_get_value_string_utf8(env, exception_string, nullptr, 0, &length) != napi_ok) {
    return "";
  }

  std::vector<char> buffer(length + 1, '\0');
  size_t copied = 0;
  if (napi_get_value_string_utf8(env, exception_string, buffer.data(), buffer.size(), &copied) != napi_ok) {
    return "";
  }
  return std::string(buffer.data(), copied);
}

std::string NapiValueToUtf8(napi_env env, napi_value value) {
  napi_value string_value = nullptr;
  if (napi_coerce_to_string(env, value, &string_value) != napi_ok || string_value == nullptr) {
    return "";
  }
  size_t length = 0;
  if (napi_get_value_string_utf8(env, string_value, nullptr, 0, &length) != napi_ok) {
    return "";
  }
  std::string out(length + 1, '\0');
  size_t copied = 0;
  if (napi_get_value_string_utf8(env, string_value, out.data(), out.size(), &copied) != napi_ok) {
    return "";
  }
  out.resize(copied);
  return out;
}

const char* DetectPlatform() {
#if defined(_WIN32)
  return "win32";
#elif defined(__APPLE__)
  return "darwin";
#elif defined(__linux__)
  return "linux";
#elif defined(__sun)
  return "sunos";
#elif defined(_AIX)
  return "aix";
#else
  return "unknown";
#endif
}

const char* DetectArch() {
#if defined(__x86_64__) || defined(_M_X64)
  return "x64";
#elif defined(__aarch64__) || defined(_M_ARM64)
  return "arm64";
#elif defined(__arm__) || defined(_M_ARM)
  return "arm";
#elif defined(__i386__) || defined(_M_IX86)
  return "ia32";
#else
  return "unknown";
#endif
}

void MaybeInvokeWriteCallback(napi_env env, napi_value maybe_fn) {
  if (maybe_fn == nullptr) return;
  napi_valuetype type = napi_undefined;
  if (napi_typeof(env, maybe_fn, &type) != napi_ok || type != napi_function) {
    return;
  }
  napi_value global = nullptr;
  napi_value null_value = nullptr;
  if (napi_get_global(env, &global) != napi_ok || global == nullptr ||
      napi_get_null(env, &null_value) != napi_ok || null_value == nullptr) {
    return;
  }
  napi_value argv[1] = {null_value};
  napi_value ignored = nullptr;
  napi_call_function(env, global, maybe_fn, 1, argv, &ignored);
}

napi_value ProcessStdoutWriteCallback(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value argv[2] = {nullptr, nullptr};
  if (napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr) == napi_ok && argc >= 1 && argv[0] != nullptr) {
    const std::string out = NapiValueToUtf8(env, argv[0]);
    std::cout << out;
    std::cout.flush();
  }
  if (argc >= 2) {
    MaybeInvokeWriteCallback(env, argv[1]);
  }
  napi_value result = nullptr;
  napi_get_boolean(env, true, &result);
  return result;
}

napi_value ProcessStderrWriteCallback(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value argv[2] = {nullptr, nullptr};
  if (napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr) == napi_ok && argc >= 1 && argv[0] != nullptr) {
    const std::string out = NapiValueToUtf8(env, argv[0]);
    std::cerr << out;
    std::cerr.flush();
  }
  if (argc >= 2) {
    MaybeInvokeWriteCallback(env, argv[1]);
  }
  napi_value result = nullptr;
  napi_get_boolean(env, true, &result);
  return result;
}

napi_status InstallProcessStream(napi_env env,
                                 napi_value process_obj,
                                 const char* name,
                                 int32_t fd,
                                 napi_callback cb) {
  napi_value stream_obj = nullptr;
  if (napi_create_object(env, &stream_obj) != napi_ok || stream_obj == nullptr) {
    return napi_generic_failure;
  }
  napi_value fd_value = nullptr;
  if (napi_create_int32(env, fd, &fd_value) != napi_ok || fd_value == nullptr) {
    return napi_generic_failure;
  }
  if (napi_set_named_property(env, stream_obj, "fd", fd_value) != napi_ok) {
    return napi_generic_failure;
  }
  napi_value true_value = nullptr;
  if (napi_get_boolean(env, true, &true_value) != napi_ok || true_value == nullptr) {
    return napi_generic_failure;
  }
  if (napi_set_named_property(env, stream_obj, "writable", true_value) != napi_ok) {
    return napi_generic_failure;
  }
  if (napi_set_named_property(env, stream_obj, "_isStdio", true_value) != napi_ok) {
    return napi_generic_failure;
  }
  napi_value false_value = nullptr;
  if (napi_get_boolean(env, false, &false_value) != napi_ok || false_value == nullptr) {
    return napi_generic_failure;
  }
  if (napi_set_named_property(env, stream_obj, "isTTY", false_value) != napi_ok) {
    return napi_generic_failure;
  }
  napi_value write_fn = nullptr;
  if (napi_create_function(env, "write", NAPI_AUTO_LENGTH, cb, nullptr, &write_fn) != napi_ok ||
      write_fn == nullptr) {
    return napi_generic_failure;
  }
  if (napi_set_named_property(env, stream_obj, "write", write_fn) != napi_ok) {
    return napi_generic_failure;
  }
  auto return_undefined = [](napi_env env, napi_callback_info info) -> napi_value {
    napi_value undefined = nullptr;
    napi_get_undefined(env, &undefined);
    return undefined;
  };
  auto return_this = [](napi_env env, napi_callback_info info) -> napi_value {
    napi_value this_arg = nullptr;
    if (napi_get_cb_info(env, info, nullptr, nullptr, &this_arg, nullptr) != napi_ok || this_arg == nullptr) {
      napi_value undefined = nullptr;
      napi_get_undefined(env, &undefined);
      return undefined;
    }
    return this_arg;
  };
  const char* event_methods_this[] = {"on", "addListener", "once", "prependListener", "removeListener"};
  for (const char* method : event_methods_this) {
    napi_value fn = nullptr;
    if (napi_create_function(env, method, NAPI_AUTO_LENGTH, return_this, nullptr, &fn) != napi_ok || fn == nullptr) {
      return napi_generic_failure;
    }
    if (napi_set_named_property(env, stream_obj, method, fn) != napi_ok) {
      return napi_generic_failure;
    }
  }
  napi_value emit_fn = nullptr;
  if (napi_create_function(env, "emit", NAPI_AUTO_LENGTH, return_undefined, nullptr, &emit_fn) != napi_ok ||
      emit_fn == nullptr) {
    return napi_generic_failure;
  }
  if (napi_set_named_property(env, stream_obj, "emit", emit_fn) != napi_ok) {
    return napi_generic_failure;
  }
  return napi_set_named_property(env, process_obj, name, stream_obj);
}

napi_value ConsoleLogCallback(napi_env env, napi_callback_info info) {
  size_t argc = 8;
  napi_value args[8] = {nullptr};
  napi_status status = napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
  if (status == napi_ok) {
    for (size_t i = 0; i < argc; ++i) {
      if (i > 0) {
        std::cout << " ";
      }
      napi_value string_value = nullptr;
      if (napi_coerce_to_string(env, args[i], &string_value) != napi_ok || string_value == nullptr) {
        continue;
      }
      size_t length = 0;
      if (napi_get_value_string_utf8(env, string_value, nullptr, 0, &length) != napi_ok) {
        continue;
      }
      std::string out(length + 1, '\0');
      size_t copied = 0;
      if (napi_get_value_string_utf8(env, string_value, out.data(), out.size(), &copied) != napi_ok) {
        continue;
      }
      out.resize(copied);
      std::cout << out;
    }
    std::cout << "\n";
  }
  napi_value undefined = nullptr;
  napi_get_undefined(env, &undefined);
  return undefined;
}

int RunScriptWithGlobals(napi_env env, const char* source_text, const char* entry_script_path, std::string* error_out) {
  if (env == nullptr) {
    if (error_out != nullptr) {
      *error_out = "Invalid environment";
    }
    return 1;
  }
  if (source_text == nullptr || source_text[0] == '\0') {
    if (error_out != nullptr) {
      *error_out = "Empty script source";
    }
    return 1;
  }

  napi_status status = UnodeInstallProcessObject(env);
  if (status != napi_ok) {
    if (error_out != nullptr) {
      *error_out = "UnodeInstallProcessObject failed: " + StatusToString(status);
    }
    return 1;
  }
  UnodeInstallFsBinding(env);
  UnodeInstallBufferBinding(env);
  UnodeInstallOsBinding(env);
  UnodeInstallEncodingBinding(env);
  UnodeInstallStringDecoderBinding(env);
  UnodeInstallUrlBinding(env);
  status = UnodeInstallModuleLoader(env, entry_script_path);
  if (status != napi_ok) {
    if (error_out != nullptr) {
      *error_out = "UnodeInstallModuleLoader failed: " + StatusToString(status);
    }
    return 1;
  }
  status = UnodeInstallConsole(env);
  if (status != napi_ok) {
    if (error_out != nullptr) {
      *error_out = "UnodeInstallConsole failed: " + StatusToString(status);
    }
    return 1;
  }

  static const char kConsoleBootstrap[] =
      "(function(){"
      "if (typeof process !== 'object' || !process) return;"
      "if (typeof process.nextTick !== 'function') {"
      "  process.nextTick = function(fn){"
      "    var args = Array.prototype.slice.call(arguments, 1);"
      "    if (typeof queueMicrotask === 'function') queueMicrotask(function(){ fn.apply(null, args); });"
      "    else if (typeof fn === 'function') fn.apply(null, args);"
      "  };"
      "}"
      "if (typeof process.emitWarning !== 'function') {"
      "  process.emitWarning = function(msg){"
      "    var text = 'Warning: ' + String(msg);"
      "    process.nextTick(function(){ if (process.stderr && typeof process.stderr.write === 'function') process.stderr.write(text + '\\n'); });"
      "  };"
      "}"
      "if (typeof process.on !== 'function') process.on = function(){ return process; };"
      "if (typeof process.addListener !== 'function') process.addListener = process.on;"
      "if (typeof process.removeListener !== 'function') process.removeListener = function(){ return process; };"
      "if (typeof process.once !== 'function') process.once = process.on;"
      "if (typeof process.platform !== 'string') process.platform = 'darwin';"
      "if (typeof process.exit !== 'function') process.exit = function(){};"
      "})();";
  napi_value bootstrap_script = nullptr;
  status = napi_create_string_utf8(env, kConsoleBootstrap, NAPI_AUTO_LENGTH, &bootstrap_script);
  if (status != napi_ok || bootstrap_script == nullptr) {
    if (error_out != nullptr) {
      *error_out = "Bootstrap script creation failed: " + StatusToString(status);
    }
    return 1;
  }
  {
    napi_value ignored = nullptr;
    status = napi_run_script(env, bootstrap_script, &ignored);
  }
  if (status != napi_ok) {
    if (error_out != nullptr) {
      *error_out = "Bootstrap script failed: " + StatusToString(status);
    }
    return 1;
  }

  // Minimal globals expected by Node test common (AbortController, timers, global, etc.).
  static const char kPrelude[] =
      "globalThis.global = globalThis;"
      "if (typeof globalThis.eval === 'function') {"
      "  var __unodeEval = globalThis.eval;"
      "  globalThis.eval = function(src) {"
      "    if (typeof src === 'string' && src.length > 0 && src[0] === '%') return undefined;"
      "    return __unodeEval(src);"
      "  };"
      "}"
      "if (typeof globalThis.AbortController === 'undefined') {"
      "  globalThis.AbortController = function AbortController() {"
      "    this.signal = { aborted: false, addEventListener: function() {} };"
      "  };"
      "}"
      "if (typeof globalThis.AbortSignal === 'undefined') {"
      "  globalThis.AbortSignal = { abort: function() { return { aborted: true }; } };"
      "}"
      "if (typeof globalThis.setImmediate === 'undefined') {"
      "  globalThis.setImmediate = function(f) {"
      "    var args = Array.prototype.slice.call(arguments, 1);"
      "    if (typeof f === 'function') globalThis.queueMicrotask(function() { f.apply(null, args); });"
      "  };"
      "}"
      "if (typeof globalThis.clearImmediate === 'undefined') {"
      "  globalThis.clearImmediate = function() {};"
      "}"
      "if (typeof globalThis.setInterval === 'undefined') {"
      "  globalThis.setInterval = function() { return 0; };"
      "}"
      "if (typeof globalThis.clearInterval === 'undefined') {"
      "  globalThis.clearInterval = function() {};"
      "}"
      "if (typeof globalThis.setTimeout === 'undefined') {"
      "  globalThis.setTimeout = function(f) { if (typeof f === 'function') f(); return 0; };"
      "}"
      "if (typeof globalThis.clearTimeout === 'undefined') {"
      "  globalThis.clearTimeout = function() {};"
      "}"
      "if (typeof globalThis.queueMicrotask === 'undefined') {"
      "  globalThis.queueMicrotask = function(f) { if (typeof f === 'function') f(); };"
      "}"
      "globalThis.__unode_detached_arraybuffers = globalThis.__unode_detached_arraybuffers || new WeakSet();"
      "var __unode_original_structuredClone = globalThis.structuredClone;"
      "globalThis.structuredClone = function(v, options) {"
      "  if (options && options.transfer && typeof options.transfer.length === 'number') {"
      "    for (var i = 0; i < options.transfer.length; i++) {"
      "      var t = options.transfer[i];"
      "      if (t && Object.prototype.toString.call(t) === '[object ArrayBuffer]') {"
      "        globalThis.__unode_detached_arraybuffers.add(t);"
      "      }"
      "    }"
      "  }"
      "  if (typeof __unode_original_structuredClone === 'function') {"
      "    return __unode_original_structuredClone(v, options);"
      "  }"
      "  return JSON.parse(JSON.stringify(v));"
      "};"
      "if (typeof globalThis.fetch === 'undefined') {"
      "  globalThis.fetch = function() { return Promise.reject(new Error('fetch not implemented')); };"
      "}"
      "if (typeof globalThis.URL === 'undefined') {"
      "  globalThis.URL = function URL(u) {"
      "    if (!(this instanceof URL)) return new URL(u);"
      "    this.href = String(u);"
      "    this.pathname = (u && u.pathname) ? u.pathname : '';"
      "  };"
      "}"
      "if (typeof globalThis.Buffer !== 'function') {"
      "  try {"
      "    var __bufferMod = require('buffer');"
      "    if (__bufferMod && typeof __bufferMod.Buffer === 'function') {"
      "      globalThis.Buffer = __bufferMod.Buffer;"
      "    }"
      "  } catch (_) {}"
      "}"
      "if (typeof globalThis.Buffer === 'undefined') {"
      "  function __encodeUtf8(s){var u=[],i=0,c;while(i<s.length){c=s.charCodeAt(i++);"
      "    if(c<128)u.push(c);else if(c<2048){u.push(192|(c>>>6));u.push(128|(c&63));}"
      "    else if(c<55296||c>57343){u.push(224|(c>>>12));u.push(128|((c>>>6)&63));u.push(128|(c&63));}"
      "    else{c=65536+((c&1023)<<10)|(s.charCodeAt(i++)&1023);"
      "      u.push(240|(c>>>18));u.push(128|((c>>>12)&63));u.push(128|((c>>>6)&63));u.push(128|(c&63));}}"
      "  return new Uint8Array(u);}"
      "  globalThis.Buffer={from:function(x){if(typeof x==='string')return __encodeUtf8(x);"
      "  if(x&&typeof x.length==='number')return new Uint8Array(x);return new Uint8Array(0);},"
      "  alloc:function(n){return new Uint8Array(n);},allocUnsafe:function(n){return new Uint8Array(n);},"
      "  byteLength:function(x){if(typeof x==='string')return __encodeUtf8(x).byteLength;"
      "  return x&&x.byteLength!==undefined?x.byteLength:0;}};"
      "}";
  napi_value prelude = nullptr;
  status = napi_create_string_utf8(env, kPrelude, NAPI_AUTO_LENGTH, &prelude);
  if (status == napi_ok && prelude != nullptr) {
    napi_value unused = nullptr;
    status = napi_run_script(env, prelude, &unused);
  }
  if (status != napi_ok) {
    if (error_out != nullptr) {
      *error_out = "Prelude failed: " + StatusToString(status);
    }
    return 1;
  }

  napi_value script = nullptr;
  status = napi_create_string_utf8(env, source_text, NAPI_AUTO_LENGTH, &script);
  if (status != napi_ok || script == nullptr) {
    if (error_out != nullptr) {
      *error_out = "napi_create_string_utf8 failed: " + StatusToString(status);
    }
    return 1;
  }

  napi_value result = nullptr;
  status = napi_run_script(env, script, &result);
  if (status == napi_ok) {
    return 0;
  }

  bool is_process_exit = false;
  int process_exit_code = 1;
  const std::string exception_message =
      GetAndClearPendingException(env, &is_process_exit, &process_exit_code);
  if (is_process_exit) {
    if (error_out != nullptr) {
      error_out->clear();
      if (process_exit_code != 0) {
        *error_out = "process.exit(" + std::to_string(process_exit_code) + ")";
      }
    }
    return process_exit_code;
  }
  if (error_out != nullptr) {
    if (!exception_message.empty()) {
      *error_out = exception_message;
    } else {
      *error_out = "napi_run_script failed: " + StatusToString(status);
    }
  }
  return 1;
}

}  // namespace

napi_status UnodeInstallConsole(napi_env env) {
  if (env == nullptr) {
    return napi_invalid_arg;
  }
  napi_value script = nullptr;
  static const char kInstallConsole[] =
      "(function(){ globalThis.console = require('console'); })();";
  napi_status status = napi_create_string_utf8(env, kInstallConsole, NAPI_AUTO_LENGTH, &script);
  if (status != napi_ok || script == nullptr) {
    return (status == napi_ok) ? napi_generic_failure : status;
  }
  napi_value ignored = nullptr;
  status = napi_run_script(env, script, &ignored);
  if (status == napi_ok) {
    return napi_ok;
  }

  // Fallback for scripts whose base directory has no JS console builtin.
  bool has_pending = false;
  if (napi_is_exception_pending(env, &has_pending) == napi_ok && has_pending) {
    napi_value exc = nullptr;
    napi_get_and_clear_last_exception(env, &exc);
  }

  napi_value global = nullptr;
  status = napi_get_global(env, &global);
  if (status != napi_ok || global == nullptr) {
    return (status == napi_ok) ? napi_generic_failure : status;
  }
  napi_value console_obj = nullptr;
  if (napi_create_object(env, &console_obj) != napi_ok || console_obj == nullptr) {
    return napi_generic_failure;
  }
  napi_value log_fn = nullptr;
  if (napi_create_function(env, "log", NAPI_AUTO_LENGTH, ConsoleLogCallback, nullptr, &log_fn) != napi_ok ||
      log_fn == nullptr) {
    return napi_generic_failure;
  }
  if (napi_set_named_property(env, console_obj, "log", log_fn) != napi_ok ||
      napi_set_named_property(env, console_obj, "info", log_fn) != napi_ok ||
      napi_set_named_property(env, console_obj, "debug", log_fn) != napi_ok) {
    return napi_generic_failure;
  }
  napi_value err_fn = nullptr;
  if (napi_create_function(env, "error", NAPI_AUTO_LENGTH, ConsoleLogCallback, nullptr, &err_fn) != napi_ok ||
      err_fn == nullptr) {
    return napi_generic_failure;
  }
  if (napi_set_named_property(env, console_obj, "error", err_fn) != napi_ok ||
      napi_set_named_property(env, console_obj, "warn", err_fn) != napi_ok) {
    return napi_generic_failure;
  }
  return napi_set_named_property(env, global, "console", console_obj);
}

napi_value ProcessOnCallback(napi_env env, napi_callback_info info) {
  napi_value undefined = nullptr;
  if (napi_get_undefined(env, &undefined) != napi_ok) {
    return nullptr;
  }
  return undefined;
}

napi_value ProcessCwdCallback(napi_env env, napi_callback_info info) {
  const char* pwd = std::getenv("PWD");
  if (pwd == nullptr || pwd[0] == '\0') {
    pwd = ".";
  }
  napi_value result = nullptr;
  if (napi_create_string_utf8(env, pwd, NAPI_AUTO_LENGTH, &result) != napi_ok) {
    return nullptr;
  }
  return result;
}

napi_value ProcessUmaskCallback(napi_env env, napi_callback_info info) {
  napi_value result = nullptr;
  if (napi_create_int32(env, 022, &result) != napi_ok) {
    return nullptr;
  }
  return result;
}

napi_value ProcessExitCallback(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value args[1] = {nullptr};
  if (napi_get_cb_info(env, info, &argc, args, nullptr, nullptr) != napi_ok) {
    return nullptr;
  }

  int32_t exit_code = 0;
  if (argc > 0 && args[0] != nullptr) {
    napi_valuetype arg_type = napi_undefined;
    if (napi_typeof(env, args[0], &arg_type) == napi_ok && arg_type != napi_undefined) {
      napi_get_value_int32(env, args[0], &exit_code);
    }
  }

  napi_value code_value = nullptr;
  napi_value message_value = nullptr;
  napi_value error_value = nullptr;
  napi_create_string_utf8(env, "ERR_UNODE_PROCESS_EXIT", NAPI_AUTO_LENGTH, &code_value);
  napi_create_string_utf8(env, "process.exit()", NAPI_AUTO_LENGTH, &message_value);
  napi_create_error(env, code_value, message_value, &error_value);
  napi_value exit_code_value = nullptr;
  napi_create_int32(env, exit_code, &exit_code_value);
  napi_set_named_property(env, error_value, "__unodeExitCode", exit_code_value);
  napi_throw(env, error_value);
  return nullptr;
}

napi_status UnodeInstallProcessObject(napi_env env) {
  if (env == nullptr) {
    return napi_invalid_arg;
  }
  napi_value global = nullptr;
  napi_status status = napi_get_global(env, &global);
  if (status != napi_ok || global == nullptr) {
    return (status == napi_ok) ? napi_generic_failure : status;
  }
  napi_value process_obj = nullptr;
  status = napi_create_object(env, &process_obj);
  if (status != napi_ok || process_obj == nullptr) {
    return (status == napi_ok) ? napi_generic_failure : status;
  }
  napi_value env_obj = nullptr;
  status = napi_create_object(env, &env_obj);
  if (status != napi_ok || env_obj == nullptr) {
    return (status == napi_ok) ? napi_generic_failure : status;
  }
  status = napi_set_named_property(env, process_obj, "env", env_obj);
  if (status != napi_ok) {
    return status;
  }
  CopyProcessEnvironmentToObject(env, env_obj);
  napi_value argv_arr = nullptr;
  status = napi_create_array_with_length(env, 0, &argv_arr);
  if (status != napi_ok || argv_arr == nullptr) {
    return (status == napi_ok) ? napi_generic_failure : status;
  }
  status = napi_set_named_property(env, process_obj, "argv", argv_arr);
  if (status != napi_ok) {
    return status;
  }
  napi_value cwd_fn = nullptr;
  status = napi_create_function(env, "cwd", NAPI_AUTO_LENGTH, ProcessCwdCallback, nullptr, &cwd_fn);
  if (status != napi_ok || cwd_fn == nullptr) {
    return (status == napi_ok) ? napi_generic_failure : status;
  }
  status = napi_set_named_property(env, process_obj, "cwd", cwd_fn);
  if (status != napi_ok) {
    return status;
  }
  napi_value umask_fn = nullptr;
  status = napi_create_function(env, "umask", NAPI_AUTO_LENGTH, ProcessUmaskCallback, nullptr, &umask_fn);
  if (status != napi_ok || umask_fn == nullptr) {
    return (status == napi_ok) ? napi_generic_failure : status;
  }
  status = napi_set_named_property(env, process_obj, "umask", umask_fn);
  if (status != napi_ok) {
    return status;
  }
  napi_value on_fn = nullptr;
  status = napi_create_function(env, "on", NAPI_AUTO_LENGTH, [](napi_env env, napi_callback_info info) {
    napi_value undefined = nullptr;
    napi_get_undefined(env, &undefined);
    return undefined;
  }, nullptr, &on_fn);
  if (status != napi_ok || on_fn == nullptr) {
    return (status == napi_ok) ? napi_generic_failure : status;
  }
  status = napi_set_named_property(env, process_obj, "on", on_fn);
  if (status != napi_ok) {
    return status;
  }
  status = napi_set_named_property(env, process_obj, "addListener", on_fn);
  if (status != napi_ok) {
    return status;
  }
  status = napi_set_named_property(env, process_obj, "once", on_fn);
  if (status != napi_ok) {
    return status;
  }
  napi_value remove_listener_fn = nullptr;
  status = napi_create_function(env, "removeListener", NAPI_AUTO_LENGTH, [](napi_env env, napi_callback_info info) {
    napi_value undefined = nullptr;
    napi_get_undefined(env, &undefined);
    return undefined;
  }, nullptr, &remove_listener_fn);
  if (status != napi_ok || remove_listener_fn == nullptr) {
    return (status == napi_ok) ? napi_generic_failure : status;
  }
  status = napi_set_named_property(env, process_obj, "removeListener", remove_listener_fn);
  if (status != napi_ok) {
    return status;
  }
  status = InstallProcessStream(env, process_obj, "stdout", 1, ProcessStdoutWriteCallback);
  if (status != napi_ok) {
    return status;
  }
  status = InstallProcessStream(env, process_obj, "stderr", 2, ProcessStderrWriteCallback);
  if (status != napi_ok) {
    return status;
  }
  napi_value exit_fn = nullptr;
  status = napi_create_function(env, "exit", NAPI_AUTO_LENGTH, ProcessExitCallback, nullptr, &exit_fn);
  if (status != napi_ok || exit_fn == nullptr) {
    return (status == napi_ok) ? napi_generic_failure : status;
  }
  status = napi_set_named_property(env, process_obj, "exit", exit_fn);
  if (status != napi_ok) {
    return status;
  }
  // process.arch/process.platform and process.versions - stubs for Node test common.
  napi_value arch_str = nullptr;
  status = napi_create_string_utf8(env, DetectArch(), NAPI_AUTO_LENGTH, &arch_str);
  if (status != napi_ok || arch_str == nullptr) {
    return (status == napi_ok) ? napi_generic_failure : status;
  }
  status = napi_set_named_property(env, process_obj, "arch", arch_str);
  if (status != napi_ok) {
    return status;
  }
  napi_value platform_str = nullptr;
  status = napi_create_string_utf8(env, DetectPlatform(), NAPI_AUTO_LENGTH, &platform_str);
  if (status != napi_ok || platform_str == nullptr) {
    return (status == napi_ok) ? napi_generic_failure : status;
  }
  status = napi_set_named_property(env, process_obj, "platform", platform_str);
  if (status != napi_ok) {
    return status;
  }
  napi_value exec_path = nullptr;
  status = napi_create_string_utf8(env, "unode", NAPI_AUTO_LENGTH, &exec_path);
  if (status != napi_ok || exec_path == nullptr) {
    return (status == napi_ok) ? napi_generic_failure : status;
  }
  status = napi_set_named_property(env, process_obj, "execPath", exec_path);
  if (status != napi_ok) {
    return status;
  }
  napi_value pid_value = nullptr;
#if defined(_WIN32)
  status = napi_create_int32(env, 1, &pid_value);
#else
  status = napi_create_int32(env, static_cast<int32_t>(getpid()), &pid_value);
#endif
  if (status != napi_ok || pid_value == nullptr) {
    return (status == napi_ok) ? napi_generic_failure : status;
  }
  status = napi_set_named_property(env, process_obj, "pid", pid_value);
  if (status != napi_ok) {
    return status;
  }
  napi_value versions_obj = nullptr;
  status = napi_create_object(env, &versions_obj);
  if (status != napi_ok || versions_obj == nullptr) {
    return (status == napi_ok) ? napi_generic_failure : status;
  }
  napi_value openssl_str = nullptr;
  status = napi_create_string_utf8(env, "3.0.0", NAPI_AUTO_LENGTH, &openssl_str);
  if (status != napi_ok || openssl_str == nullptr) {
    return (status == napi_ok) ? napi_generic_failure : status;
  }
  status = napi_set_named_property(env, versions_obj, "openssl", openssl_str);
  if (status != napi_ok) {
    return status;
  }
  status = napi_set_named_property(env, process_obj, "versions", versions_obj);
  if (status != napi_ok) {
    return status;
  }
  napi_value features_obj = nullptr;
  status = napi_create_object(env, &features_obj);
  if (status != napi_ok || features_obj == nullptr) {
    return (status == napi_ok) ? napi_generic_failure : status;
  }
  napi_value inspector_true = nullptr;
  status = napi_get_boolean(env, true, &inspector_true);
  if (status != napi_ok || inspector_true == nullptr) {
    return (status == napi_ok) ? napi_generic_failure : status;
  }
  status = napi_set_named_property(env, features_obj, "inspector", inspector_true);
  if (status != napi_ok) {
    return status;
  }
  status = napi_set_named_property(env, process_obj, "features", features_obj);
  if (status != napi_ok) {
    return status;
  }
  // process.config.variables - minimal stub for Node test common (e.g. hasIntl, hasQuic, isASan).
  napi_value config_obj = nullptr;
  status = napi_create_object(env, &config_obj);
  if (status != napi_ok || config_obj == nullptr) {
    return (status == napi_ok) ? napi_generic_failure : status;
  }
  napi_value variables_obj = nullptr;
  status = napi_create_object(env, &variables_obj);
  if (status != napi_ok || variables_obj == nullptr) {
    return (status == napi_ok) ? napi_generic_failure : status;
  }
  napi_value zero = nullptr;
  status = napi_create_int32(env, 0, &zero);
  if (status != napi_ok || zero == nullptr) {
    return (status == napi_ok) ? napi_generic_failure : status;
  }
  const char* var_keys[] = {"v8_enable_i18n_support", "node_quic", "asan"};
  for (const char* key : var_keys) {
    if (napi_set_named_property(env, variables_obj, key, zero) != napi_ok) {
      return napi_generic_failure;
    }
  }
  status = napi_set_named_property(env, config_obj, "variables", variables_obj);
  if (status != napi_ok) {
    return status;
  }
  status = napi_set_named_property(env, process_obj, "config", config_obj);
  if (status != napi_ok) {
    return status;
  }
  return napi_set_named_property(env, global, "process", process_obj);
}

int UnodeRunScriptSource(napi_env env, const char* source_text, std::string* error_out) {
  if (error_out != nullptr) {
    error_out->clear();
  }
  return RunScriptWithGlobals(env, source_text, nullptr, error_out);
}

int UnodeRunScriptFile(napi_env env, const char* script_path, std::string* error_out) {
  if (error_out != nullptr) {
    error_out->clear();
  }
  const std::string source = ReadTextFile(script_path);
  if (source.empty()) {
    if (error_out != nullptr) {
      *error_out = "Failed to read script file";
    }
    return 1;
  }
  return RunScriptWithGlobals(env, source.c_str(), script_path, error_out);
}
