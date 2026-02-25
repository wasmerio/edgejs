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
extern char** environ;
#endif

#include "unode_fs.h"
#include "unode_module_loader.h"

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

std::string GetAndClearPendingException(napi_env env) {
  bool pending = false;
  if (napi_is_exception_pending(env, &pending) != napi_ok || !pending) {
    return "";
  }

  napi_value exception = nullptr;
  if (napi_get_and_clear_last_exception(env, &exception) != napi_ok || exception == nullptr) {
    return "";
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

  napi_status status = UnodeInstallConsole(env);
  if (status != napi_ok) {
    if (error_out != nullptr) {
      *error_out = "UnodeInstallConsole failed: " + StatusToString(status);
    }
    return 1;
  }
  status = UnodeInstallProcessObject(env);
  if (status != napi_ok) {
    if (error_out != nullptr) {
      *error_out = "UnodeInstallProcessObject failed: " + StatusToString(status);
    }
    return 1;
  }
  UnodeInstallFsBinding(env);
  status = UnodeInstallModuleLoader(env, entry_script_path);
  if (status != napi_ok) {
    if (error_out != nullptr) {
      *error_out = "UnodeInstallModuleLoader failed: " + StatusToString(status);
    }
    return 1;
  }

  // Minimal globals expected by Node test common (AbortController, timers, global, etc.).
  static const char kPrelude[] =
      "globalThis.global = globalThis;"
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
      "if (typeof globalThis.structuredClone === 'undefined') {"
      "  globalThis.structuredClone = function(v) { return JSON.parse(JSON.stringify(v)); };"
      "}"
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

  const std::string exception_message = GetAndClearPendingException(env);
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

  napi_value global = nullptr;
  napi_status status = napi_get_global(env, &global);
  if (status != napi_ok || global == nullptr) {
    return (status == napi_ok) ? napi_generic_failure : status;
  }

  napi_value console_obj = nullptr;
  bool has_console = false;
  status = napi_has_named_property(env, global, "console", &has_console);
  if (status != napi_ok) return status;
  if (has_console) {
    status = napi_get_named_property(env, global, "console", &console_obj);
    if (status != napi_ok || console_obj == nullptr) {
      return (status == napi_ok) ? napi_generic_failure : status;
    }
  } else {
    status = napi_create_object(env, &console_obj);
    if (status != napi_ok || console_obj == nullptr) {
      return (status == napi_ok) ? napi_generic_failure : status;
    }
  }
  napi_value log_fn = nullptr;
  status = napi_create_function(env, "log", NAPI_AUTO_LENGTH, ConsoleLogCallback, nullptr, &log_fn);
  if (status != napi_ok || log_fn == nullptr) {
    return (status == napi_ok) ? napi_generic_failure : status;
  }
  status = napi_set_named_property(env, console_obj, "log", log_fn);
  if (status != napi_ok) {
    return status;
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
  // process.arch and process.versions - stubs for Node test common.
  napi_value arch_str = nullptr;
  status = napi_create_string_utf8(env, "arm64", NAPI_AUTO_LENGTH, &arch_str);
  if (status != napi_ok || arch_str == nullptr) {
    return (status == napi_ok) ? napi_generic_failure : status;
  }
  status = napi_set_named_property(env, process_obj, "arch", arch_str);
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
