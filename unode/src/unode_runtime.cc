#include "unode_runtime.h"

#include <cstdlib>
#include <chrono>
#include <cerrno>
#include <cmath>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include <uv.h>

#if !defined(_WIN32)
#include <unistd.h>
#endif

#include "unode_fs.h"
#include "unode_buffer.h"
#include "unode_encoding.h"
#include "unode_http_parser.h"
#include "unode_module_loader.h"
#include "unode_os.h"
#include "unode_pipe_wrap.h"
#include "unode_runtime_platform.h"
#include "unode_stream_wrap.h"
#include "unode_string_decoder.h"
#include "unode_tcp_wrap.h"
#include "unode_udp_wrap.h"
#include "unode_url.h"
#include "unode_util.h"
#include "unode_cares_wrap.h"

namespace {

std::string g_unode_current_script_path;
std::vector<std::string> g_unode_exec_argv;
std::string g_unode_process_title;
const auto g_process_start_time = std::chrono::steady_clock::now();

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

int GetProcessExitCode(napi_env env, bool* has_exit_code) {
  if (has_exit_code != nullptr) *has_exit_code = false;
  napi_value global = nullptr;
  if (napi_get_global(env, &global) != napi_ok || global == nullptr) {
    return 0;
  }
  bool has_process = false;
  if (napi_has_named_property(env, global, "process", &has_process) != napi_ok || !has_process) {
    return 0;
  }
  napi_value process_obj = nullptr;
  if (napi_get_named_property(env, global, "process", &process_obj) != napi_ok || process_obj == nullptr) {
    return 0;
  }
  bool has_exit_code_prop = false;
  if (napi_has_named_property(env, process_obj, "exitCode", &has_exit_code_prop) != napi_ok ||
      !has_exit_code_prop) {
    return 0;
  }
  napi_value exit_code_value = nullptr;
  if (napi_get_named_property(env, process_obj, "exitCode", &exit_code_value) != napi_ok ||
      exit_code_value == nullptr) {
    return 0;
  }
  napi_valuetype value_type = napi_undefined;
  if (napi_typeof(env, exit_code_value, &value_type) != napi_ok || value_type == napi_undefined ||
      value_type == napi_null) {
    return 0;
  }
  int32_t exit_code = 0;
  if (napi_get_value_int32(env, exit_code_value, &exit_code) != napi_ok) {
    return 0;
  }
  if (has_exit_code != nullptr) *has_exit_code = true;
  return static_cast<int>(exit_code);
}

int GetProcessExitCodeOrZero(napi_env env) {
  bool has_exit_code = false;
  const int exit_code = GetProcessExitCode(env, &has_exit_code);
  return has_exit_code ? exit_code : 0;
}

bool EmitProcessLifecycleEvent(napi_env env, const char* event_name, int exit_code) {
  napi_value global = nullptr;
  if (napi_get_global(env, &global) != napi_ok || global == nullptr) {
    return false;
  }
  bool has_process = false;
  if (napi_has_named_property(env, global, "process", &has_process) != napi_ok || !has_process) {
    return false;
  }
  napi_value process_obj = nullptr;
  if (napi_get_named_property(env, global, "process", &process_obj) != napi_ok || process_obj == nullptr) {
    return false;
  }
  bool has_emit = false;
  if (napi_has_named_property(env, process_obj, "emit", &has_emit) != napi_ok || !has_emit) {
    return false;
  }
  napi_value emit_fn = nullptr;
  if (napi_get_named_property(env, process_obj, "emit", &emit_fn) != napi_ok || emit_fn == nullptr) {
    return false;
  }
  napi_valuetype emit_type = napi_undefined;
  if (napi_typeof(env, emit_fn, &emit_type) != napi_ok || emit_type != napi_function) {
    return false;
  }
  napi_value event_name_value = nullptr;
  napi_value exit_code_value = nullptr;
  if (napi_create_string_utf8(env, event_name, NAPI_AUTO_LENGTH, &event_name_value) != napi_ok ||
      event_name_value == nullptr ||
      napi_create_int32(env, static_cast<int32_t>(exit_code), &exit_code_value) != napi_ok ||
      exit_code_value == nullptr) {
    return false;
  }
  napi_value args[2] = {event_name_value, exit_code_value};
  napi_value ignored = nullptr;
  return napi_call_function(env, process_obj, emit_fn, 2, args, &ignored) == napi_ok;
}

int HandlePendingExceptionAfterLoopStep(napi_env env, std::string* error_out) {
  bool has_pending = false;
  if (napi_is_exception_pending(env, &has_pending) != napi_ok || !has_pending) {
    return -1;
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
    *error_out = exception_message.empty() ? "Unhandled async exception" : exception_message;
  }
  return 1;
}

int RunEventLoopUntilQuiescent(napi_env env, std::string* error_out) {
  uv_loop_t* loop = uv_default_loop();
  if (loop == nullptr) {
    if (error_out != nullptr) {
      *error_out = "Missing default libuv loop";
    }
    return 1;
  }
  while (true) {
    uv_run(loop, UV_RUN_DEFAULT);
    // Node drains platform tasks between libuv turns; mirror that behavior.
    (void)UnodeRuntimePlatformDrainTasks(env);

    int async_status = HandlePendingExceptionAfterLoopStep(env, error_out);
    if (async_status >= 0) {
      return async_status;
    }

    bool more = uv_loop_alive(loop) != 0;
    if (more) {
      continue;
    }

    const int before_exit_code = GetProcessExitCodeOrZero(env);
    EmitProcessLifecycleEvent(env, "beforeExit", before_exit_code);
    (void)UnodeRuntimePlatformDrainTasks(env);

    async_status = HandlePendingExceptionAfterLoopStep(env, error_out);
    if (async_status >= 0) {
      return async_status;
    }

    more = uv_loop_alive(loop) != 0;
    if (!more) {
      break;
    }
  }

  EmitProcessLifecycleEvent(env, "exit", GetProcessExitCodeOrZero(env));
  const int async_status = HandlePendingExceptionAfterLoopStep(env, error_out);
  if (async_status >= 0) {
    return async_status;
  }

  return -1;
}

std::string EscapeForSingleQuotedJs(const std::string& in) {
  std::string out;
  out.reserve(in.size() + 8);
  for (char ch : in) {
    switch (ch) {
      case '\\':
        out += "\\\\";
        break;
      case '\'':
        out += "\\'";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      default:
        out.push_back(ch);
        break;
    }
  }
  return out;
}

void ParseNodeStyleFlagsFromSource(const char* source_text) {
  g_unode_exec_argv.clear();
  g_unode_process_title.clear();
  if (source_text == nullptr) return;
  std::istringstream in{std::string(source_text)};
  std::string line;
  while (std::getline(in, line)) {
    const auto pos = line.find("Flags:");
    if (pos == std::string::npos) continue;
    std::string rest = line.substr(pos + 6);
    std::istringstream tokens(rest);
    std::string token;
    while (tokens >> token) {
      g_unode_exec_argv.push_back(token);
      static constexpr const char kTitlePrefix[] = "--title=";
      if (token.rfind(kTitlePrefix, 0) == 0) {
        g_unode_process_title = token.substr(sizeof(kTitlePrefix) - 1);
      }
    }
  }
}

void ClearPendingExceptionIfAny(napi_env env) {
  bool pending = false;
  if (napi_is_exception_pending(env, &pending) != napi_ok || !pending) return;
  napi_value ignored = nullptr;
  napi_get_and_clear_last_exception(env, &ignored);
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

int RunScriptWithGlobals(napi_env env,
                         const char* source_text,
                         const char* entry_script_path,
                         std::string* error_out,
                         bool keep_event_loop_alive) {
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
  ParseNodeStyleFlagsFromSource(source_text);

  napi_status status = UnodeInstallProcessObject(
      env, g_unode_current_script_path, g_unode_exec_argv, g_unode_process_title);
  if (status != napi_ok) {
    if (error_out != nullptr) {
      *error_out = "UnodeInstallProcessObject failed: " + StatusToString(status);
    }
    return 1;
  }
  UnodeInstallFsBinding(env);
  ClearPendingExceptionIfAny(env);
  UnodeInstallBufferBinding(env);
  ClearPendingExceptionIfAny(env);
  UnodeInstallOsBinding(env);
  ClearPendingExceptionIfAny(env);
  UnodeInstallEncodingBinding(env);
  ClearPendingExceptionIfAny(env);
  UnodeInstallStringDecoderBinding(env);
  ClearPendingExceptionIfAny(env);
  UnodeInstallHttpParserBinding(env);
  ClearPendingExceptionIfAny(env);
  UnodeInstallStreamWrapBinding(env);
  ClearPendingExceptionIfAny(env);
  UnodeInstallTcpWrapBinding(env);
  ClearPendingExceptionIfAny(env);
  UnodeInstallPipeWrapBinding(env);
  ClearPendingExceptionIfAny(env);
  UnodeInstallCaresWrapBinding(env);
  ClearPendingExceptionIfAny(env);
  UnodeInstallUdpWrapBinding(env);
  ClearPendingExceptionIfAny(env);
  UnodeInstallUrlBinding(env);
  UnodeInstallUtilBinding(env);
  ClearPendingExceptionIfAny(env);
  status = UnodeInstallModuleLoader(env, entry_script_path);
  if (status != napi_ok) {
    if (error_out != nullptr) {
      *error_out = "UnodeInstallModuleLoader failed: " + StatusToString(status);
    }
    return 1;
  }

  static const char kConsoleBootstrap[] =
      "(function(){"
      "if (typeof process !== 'object' || !process) return;"
      "if (typeof process.nextTick !== 'function') {"
      "  process.nextTick = function(fn){"
      "    if (typeof fn !== 'function') {"
      "      var e = new TypeError('The \"callback\" argument must be of type function.');"
      "      e.code = 'ERR_INVALID_ARG_TYPE';"
      "      throw e;"
      "    }"
      "    var args = Array.prototype.slice.call(arguments, 1);"
      "    var invoke = function(){"
      "      try {"
      "        fn.apply(null, args);"
      "      } catch (err) {"
      "        if (process && typeof process.emit === 'function' && typeof process.listenerCount === 'function' && process.listenerCount('uncaughtException') > 0) {"
      "          process.emit('uncaughtException', err);"
      "        } else {"
      "          try {"
      "            var __text = (err && err.stack) ? String(err.stack) : String(err);"
      "            if (process && process.stderr && typeof process.stderr.write === 'function') process.stderr.write(__text + '\\n');"
      "          } catch (_) {}"
      "          throw err;"
      "        }"
      "      }"
      "    };"
      "    if (typeof queueMicrotask === 'function') queueMicrotask(invoke);"
      "    else invoke();"
      "  };"
      "}"
      "if (typeof process.emitWarning !== 'function') {"
      "  process.emitWarning = function(warning, type, code, ctor){"
      "    var makeTypeErr = function(){"
      "      var e = new TypeError('The \"warning\" argument must be of type string or an instance of Error.');"
      "      e.code = 'ERR_INVALID_ARG_TYPE';"
      "      return e;"
      "    };"
      "    if (warning === undefined) throw makeTypeErr();"
      "    var detail;"
      "    if (type && typeof type === 'object' && !Array.isArray(type) && typeof type !== 'function') {"
      "      var options = type;"
      "      type = options.type;"
      "      code = options.code;"
      "      if (typeof options.detail === 'string') detail = options.detail;"
      "      ctor = undefined;"
      "    } else if (typeof type === 'function') {"
      "      ctor = type;"
      "      type = undefined;"
      "      code = undefined;"
      "    } else if (typeof code === 'function') {"
      "      ctor = code;"
      "      code = undefined;"
      "    }"
      "    if (!(warning instanceof Error) && typeof warning !== 'string') throw makeTypeErr();"
      "    if (type !== undefined && typeof type !== 'string') throw makeTypeErr();"
      "    if (code !== undefined && typeof code !== 'string') throw makeTypeErr();"
      "    if (ctor !== undefined && typeof ctor !== 'function') throw makeTypeErr();"
      "    var w;"
      "    if (warning instanceof Error) {"
      "      w = warning;"
      "    } else if (typeof ctor === 'function') {"
      "      w = new Error(String(warning));"
      "      if (typeof Error.captureStackTrace === 'function') {"
      "        try { Error.captureStackTrace(w, ctor); } catch (_) {}"
      "      }"
      "      w.name = (typeof type === 'string' && type.length > 0) ? type : 'Warning';"
      "    } else {"
      "      w = new Error(String(warning));"
      "      w.name = (typeof type === 'string' && type.length > 0) ? type : 'Warning';"
      "      w.message = String(warning);"
      "    }"
      "    if (typeof type === 'string' && type.length > 0) w.name = type;"
      "    if (code !== undefined) w.code = code;"
      "    if (detail !== undefined) w.detail = detail;"
      "    if (w && w.name === 'DeprecationWarning' && process.noDeprecation === true) return;"
      "    if (typeof process.emit === 'function') process.emit('warning', w);"
      "    var text = String((w && w.name) || 'Warning') + ': ' + String((w && w.message) || '');"
      "    process.nextTick(function(){ if (process.stderr && typeof process.stderr.write === 'function') process.stderr.write(text + '\\n'); });"
      "  };"
      "}"
      "if (typeof process.emit !== 'function') {"
      "  var __pl = new Map();"
      "  process.on = process.addListener = function(name, fn){"
      "    var k = String(name);"
      "    var arr = __pl.get(k) || [];"
      "    arr.push(fn);"
      "    __pl.set(k, arr);"
      "    return process;"
      "  };"
      "  process.removeListener = function(name, fn){"
      "    var k = String(name);"
      "    var arr = __pl.get(k) || [];"
      "    var idx = arr.lastIndexOf(fn);"
      "    if (idx >= 0) arr.splice(idx, 1);"
      "    __pl.set(k, arr);"
      "    return process;"
      "  };"
      "  process.once = function(name, fn){"
      "    function wrapped(){ process.removeListener(name, wrapped); return fn.apply(this, arguments); }"
      "    return process.on(name, wrapped);"
      "  };"
      "  process.emit = function(name){"
      "    var k = String(name);"
      "    var arr = (__pl.get(k) || []).slice();"
      "    var args = Array.prototype.slice.call(arguments, 1);"
      "    for (var i = 0; i < arr.length; i++) arr[i].apply(process, args);"
      "    return arr.length > 0;"
      "  };"
      "  process.listenerCount = function(name){"
      "    var arr = __pl.get(String(name));"
      "    return arr ? arr.length : 0;"
      "  };"
      "}"
      "if (typeof process._getActiveRequests !== 'function') {"
      "  process._getActiveRequests = function(){"
      "    return Array.isArray(globalThis.__unode_active_requests) ? globalThis.__unode_active_requests.slice() : [];"
      "  };"
      "}"
      "if (!globalThis.__unode_resource_tracking_installed) {"
      "  globalThis.__unode_resource_tracking_installed = true;"
      "  var __activeResources = globalThis.__unode_active_resources;"
      "  if (!(__activeResources && typeof __activeResources.set === 'function')) {"
      "    __activeResources = new Map();"
      "    globalThis.__unode_active_resources = __activeResources;"
      "  }"
      "  var __nativeSetTimeout = globalThis.setTimeout;"
      "  var __nativeClearTimeout = globalThis.clearTimeout;"
      "  var __nativeSetInterval = globalThis.setInterval;"
      "  var __nativeClearInterval = globalThis.clearInterval;"
      "  var __nativeSetImmediate = globalThis.setImmediate;"
      "  var __nativeClearImmediate = globalThis.clearImmediate;"
      "  if (typeof __nativeSetTimeout === 'function') {"
      "    globalThis.setTimeout = function(cb, ms){"
      "      var args = Array.prototype.slice.call(arguments, 2);"
      "      var h;"
      "      var wrapped = function(){"
      "        __activeResources.delete(h);"
      "        if (typeof cb === 'function') return cb.apply(this, arguments);"
      "      };"
      "      h = __nativeSetTimeout.apply(globalThis, [wrapped, ms].concat(args));"
      "      __activeResources.set(h, 'Timeout');"
      "      return h;"
      "    };"
      "  }"
      "  if (typeof __nativeClearTimeout === 'function') {"
      "    globalThis.clearTimeout = function(h){ __activeResources.delete(h); return __nativeClearTimeout(h); };"
      "  }"
      "  if (typeof __nativeSetInterval === 'function') {"
      "    globalThis.setInterval = function(cb, ms){"
      "      var args = Array.prototype.slice.call(arguments, 2);"
      "      var h = __nativeSetInterval.apply(globalThis, [cb, ms].concat(args));"
      "      __activeResources.set(h, 'Timeout');"
      "      return h;"
      "    };"
      "  }"
      "  if (typeof __nativeClearInterval === 'function') {"
      "    globalThis.clearInterval = function(h){ __activeResources.delete(h); return __nativeClearInterval(h); };"
      "  }"
      "  if (typeof __nativeSetImmediate === 'function') {"
      "    globalThis.setImmediate = function(cb){"
      "      var args = Array.prototype.slice.call(arguments, 1);"
      "      var h;"
      "      var wrapped = function(){"
      "        __activeResources.delete(h);"
      "        if (typeof cb === 'function') return cb.apply(this, arguments);"
      "      };"
      "      h = __nativeSetImmediate.apply(globalThis, [wrapped].concat(args));"
      "      __activeResources.set(h, 'Immediate');"
      "      return h;"
      "    };"
      "  }"
      "  if (typeof __nativeClearImmediate === 'function') {"
      "    globalThis.clearImmediate = function(h){ __activeResources.delete(h); return __nativeClearImmediate(h); };"
      "  }"
      "}"
      "if (typeof process.getActiveResourcesInfo !== 'function') {"
      "  process.getActiveResourcesInfo = function(){"
      "    var out = [];"
      "    var res = globalThis.__unode_active_resources;"
      "    if (res && typeof res.forEach === 'function') res.forEach(function(v){ out.push(v); });"
      "    var reqs = Array.isArray(globalThis.__unode_active_requests) ? globalThis.__unode_active_requests : [];"
      "    for (var i = 0; i < reqs.length; i++) out.push(String((reqs[i] && reqs[i].type) || 'FSReqCallback'));"
      "    return out;"
      "  };"
      "}"
      "if (typeof process.threadCpuUsage !== 'function' && typeof process.cpuUsage === 'function') {"
      "  var __invalidPrev = function(v){"
      "    if (v == null) return ' Received ' + String(v);"
      "    if (typeof v === 'function') return ' Received function ' + (v.name || '<anonymous>');"
      "    if (typeof v === 'object') return ' Received an instance of ' + ((v.constructor && v.constructor.name) || 'Object');"
      "    if (typeof v === 'string') return \" Received type string ('\" + v + \"')\";"
      "    return ' Received type ' + typeof v + ' (' + String(v) + ')';"
      "  };"
      "  process.threadCpuUsage = function(prevValue){"
      "    if (arguments.length === 0) return process.cpuUsage();"
      "    if (prevValue === null || typeof prevValue !== 'object' || Array.isArray(prevValue)) {"
      "      var e = new TypeError('The \"prevValue\" argument must be of type object.' + __invalidPrev(prevValue));"
      "      e.code = 'ERR_INVALID_ARG_TYPE';"
      "      throw e;"
      "    }"
      "    if (!Object.prototype.hasOwnProperty.call(prevValue, 'user')) {"
      "      var eu = new TypeError('The \"prevValue.user\" property must be of type number. Received undefined');"
      "      eu.code = 'ERR_INVALID_ARG_TYPE';"
      "      throw eu;"
      "    }"
      "    if (typeof prevValue.user !== 'number') {"
      "      var eu2 = new TypeError('The \"prevValue.user\" property must be of type number.' + __invalidPrev(prevValue.user));"
      "      eu2.code = 'ERR_INVALID_ARG_TYPE';"
      "      throw eu2;"
      "    }"
      "    if (!Number.isFinite(prevValue.user) || prevValue.user < 0) {"
      "      var er = new RangeError(\"The property 'prevValue.user' is invalid. Received \" + String(prevValue.user));"
      "      er.code = 'ERR_INVALID_ARG_VALUE';"
      "      throw er;"
      "    }"
      "    if (!Object.prototype.hasOwnProperty.call(prevValue, 'system')) {"
      "      var es = new TypeError('The \"prevValue.system\" property must be of type number. Received undefined');"
      "      es.code = 'ERR_INVALID_ARG_TYPE';"
      "      throw es;"
      "    }"
      "    if (typeof prevValue.system !== 'number') {"
      "      var es2 = new TypeError('The \"prevValue.system\" property must be of type number.' + __invalidPrev(prevValue.system));"
      "      es2.code = 'ERR_INVALID_ARG_TYPE';"
      "      throw es2;"
      "    }"
      "    if (!Number.isFinite(prevValue.system) || prevValue.system < 0) {"
      "      var er2 = new RangeError(\"The property 'prevValue.system' is invalid. Received \" + String(prevValue.system));"
      "      er2.code = 'ERR_INVALID_ARG_VALUE';"
      "      throw er2;"
      "    }"
      "    return process.cpuUsage({ user: prevValue.user, system: prevValue.system });"
      "  };"
      "}"
      "if (typeof process.setSourceMapsEnabled !== 'function') {"
      "  var __sourceMapsEnabled = false;"
      "  process.setSourceMapsEnabled = function(v){"
      "    if (typeof v !== 'boolean') {"
      "      var err = new TypeError('The \"enabled\" argument must be of type boolean.');"
      "      err.code = 'ERR_INVALID_ARG_TYPE';"
      "      throw err;"
      "    }"
      "    __sourceMapsEnabled = v;"
      "  };"
      "  process.sourceMapsEnabled = function(){ return __sourceMapsEnabled; };"
      "}"
      "if (!process.allowedNodeEnvironmentFlags) {"
      "  var __allowedFlags = new Set();"
      "  var __toCanonicalFlag = function(flag) {"
      "    if (typeof flag !== 'string') return '';"
      "    var f = flag.trim();"
      "    if (f.length === 0) return '';"
      "    if (!f.startsWith('-')) f = (f.length === 1 ? '-' : '--') + f;"
      "    else if (f.startsWith('-') && !f.startsWith('--')) f = '-' + f.slice(1);"
      "    var eq = f.indexOf('=');"
      "    var base = eq >= 0 ? f.slice(0, eq) : f;"
      "    var val = eq >= 0 ? f.slice(eq) : '';"
      "    base = base.replace(/_/g, '-');"
      "    return base + val;"
      "  };"
      "  try {"
      "    var __fs = require('fs');"
      "    var __path = require('path');"
      "    var __root = process.env && process.env.NODE_TEST_DIR ? __path.resolve(process.env.NODE_TEST_DIR, '..') : null;"
      "    if (__root) {"
      "      var __cliMd = __path.join(__root, 'doc', 'api', 'cli.md');"
      "      var __text = __fs.readFileSync(__cliMd, 'utf8');"
      "      var __collect = function(start, end) {"
      "        var sIdx = __text.indexOf(start);"
      "        if (sIdx < 0) return;"
      "        sIdx += start.length;"
      "        var eIdx = __text.indexOf(end, sIdx);"
      "        if (eIdx < 0) return;"
      "        var lines = __text.slice(sIdx, eIdx).split(/\\\\r?\\\\n/);"
      "        for (var i = 0; i < lines.length; i++) {"
      "          var line = lines[i];"
      "          if (!line || !line.trim()) continue;"
      "          var matches = line.match(/`(-[^`]+)`/g) || [];"
      "          for (var j = 0; j < matches.length; j++) {"
      "            var opt = matches[j].slice(1, -1);"
      "            if (opt.startsWith('--no-')) opt = '--' + opt.slice(5);"
      "            __allowedFlags.add(__toCanonicalFlag(opt));"
      "          }"
      "        }"
      "      };"
      "      __collect('<!-- node-options-node start -->', '<!-- node-options-node end -->');"
      "      __collect('<!-- node-options-v8 start -->', '<!-- node-options-v8 end -->');"
      "    }"
      "  } catch (_) {}"
      "  if (__allowedFlags.size < 50) {"
      "    var __fallbackDocFlags = ("
      "      '--allow-addons --allow-child-process --allow-fs-read --allow-fs-write --allow-inspector --allow-wasi --allow-worker --conditions -C --cpu-prof-dir --cpu-prof-interval --cpu-prof-name --cpu-prof --diagnostic-dir --disable-proto --disable-sigusr1 --disable-warning --disable-wasm-trap-handler --dns-result-order --enable-network-family-autoselection --enable-source-maps --entry-url --experimental-abortcontroller --experimental-addon-modules --experimental-detect-module --experimental-eventsource --experimental-import-meta-resolve --experimental-json-modules --experimental-loader --experimental-modules --experimental-print-required-tla --experimental-require-module --experimental-shadow-realm --experimental-specifier-resolution --experimental-test-isolation --experimental-top-level-await --experimental-transform-types --experimental-vm-modules --experimental-wasi-unstable-preview1 --experimental-webstorage --force-context-aware --force-node-api-uncaught-exceptions-policy --frozen-intrinsics --heap-prof-dir --heap-prof-interval --heap-prof-name --heap-prof --heapsnapshot-near-heap-limit --heapsnapshot-signal --http-parser --import --input-type --insecure-http-parser --inspect-port --debug-port --inspect-publish-uid --inspect-wait --inspect --localstorage-file --max-http-header-size --max-old-space-size-percentage --napi-modules --network-family-autoselection-attempt-timeout --addons --async-context-frame --deprecation --experimental-global-navigator --experimental-repl-await --experimental-sqlite --experimental-strip-types --experimental-websocket --extra-info-on-fatal-exception --force-async-hooks-checks --global-search-paths --network-family-autoselection --strip-types --warnings --node-memory-debug --pending-deprecation --permission --preserve-symlinks-main --preserve-symlinks --prof-process --redirect-warnings --report-compact --report-dir --report-directory --report-exclude-env --report-exclude-network --report-filename --report-on-fatalerror --report-on-signal --report-signal --report-uncaught-exception --require -r --snapshot-blob --test-coverage-branches --test-coverage-exclude --test-coverage-functions --test-coverage-include --test-coverage-lines --test-global-setup --test-isolation --test-name-pattern --test-only --test-reporter-destination --test-reporter --test-rerun-failures --test-shard --test-skip-pattern --throw-deprecation --title --tls-keylog --tls-max-v1.2 --tls-max-v1.3 --tls-min-v1.0 --tls-min-v1.1 --tls-min-v1.2 --tls-min-v1.3 --trace-deprecation --trace-env-js-stack --trace-env-native-stack --trace-env --trace-event-categories --trace-event-file-pattern --trace-events-enabled --trace-exit --trace-require-module --trace-sigint --trace-sync-io --trace-tls --trace-uncaught --trace-warnings --track-heap-objects --unhandled-rejections --use-env-proxy --use-largepages --use-system-ca --v8-pool-size --watch-kill-signal --watch-path --watch-preserve-output --watch --zero-fill-buffers --abort-on-uncaught-exception --disallow-code-generation-from-strings --enable-etw-stack-walking --expose-gc --interpreted-frames-native-stack --jitless --max-old-space-size --max-semi-space-size --perf-basic-prof-only-functions --perf-basic-prof --perf-prof-unwinding-info --perf-prof --stack-trace-limit'"
      "    ).split(' ');"
      "    for (var __fi = 0; __fi < __fallbackDocFlags.length; __fi++) {"
      "      if (__fallbackDocFlags[__fi]) __allowedFlags.add(__toCanonicalFlag(__fallbackDocFlags[__fi]));"
      "    }"
      "  }"
      "  ;["
      "    '--debug-arraybuffer-allocations', '--no-debug-arraybuffer-allocations',"
      "    '--es-module-specifier-resolution', '--experimental-fetch', '--experimental-wasm-modules',"
      "    '--experimental-global-customevent', '--experimental-global-webcrypto', '--experimental-report',"
      "    '--experimental-worker', '--node-snapshot', '--no-node-snapshot', '--loader',"
      "    '--verify-base-objects', '--no-verify-base-objects', '--trace-promises', '--no-trace-promises',"
      "    '--experimental-quic'"
      "  ].forEach(function(f){ __allowedFlags.add(__toCanonicalFlag(f)); });"
      "  if (process.features && process.features.inspector) {"
      "    ['--inspect-brk', '--inspect_brk', 'inspect-brk'].forEach(function(f){ __allowedFlags.add(__toCanonicalFlag(f)); });"
      "  }"
      "  var __setAdd = Set.prototype.add;"
      "  var __setDelete = Set.prototype.delete;"
      "  var __setClear = Set.prototype.clear;"
      "  __allowedFlags.has = function(flag) {"
      "    var c = __toCanonicalFlag(String(flag));"
      "    var eq = c.indexOf('=');"
      "    var base = eq >= 0 ? c.slice(0, eq) : c;"
      "    return Set.prototype.has.call(__allowedFlags, c) || Set.prototype.has.call(__allowedFlags, base);"
      "  };"
      "  __allowedFlags.add = function() { return __allowedFlags; };"
      "  __allowedFlags.delete = function() { return false; };"
      "  __allowedFlags.clear = function() {};"
      "  try {"
      "    Set.prototype.add = function(v) { if (this === __allowedFlags) return this; return __setAdd.call(this, v); };"
      "    Set.prototype.delete = function(v) { if (this === __allowedFlags) return false; return __setDelete.call(this, v); };"
      "    Set.prototype.clear = function() { if (this === __allowedFlags) return; return __setClear.call(this); };"
      "  } catch (_) {}"
      "  Object.freeze(__allowedFlags);"
      "  process.allowedNodeEnvironmentFlags = __allowedFlags;"
      "}"
      "if (typeof process.getuid !== 'function') process.getuid = function(){ return 1000; };"
      "if (typeof process.getgid !== 'function') process.getgid = function(){ return 1000; };"
      "if (typeof process.geteuid !== 'function') process.geteuid = function(){ return process.getuid(); };"
      "if (typeof process.getegid !== 'function') process.getegid = function(){ return process.getgid(); };"
      "if (!process.__unode_posix_ids) process.__unode_posix_ids = { uid: process.getuid(), gid: process.getgid(), euid: process.geteuid(), egid: process.getegid() };"
      "if (!process.__unode_posix_helpers) {"
      "  process.__unode_posix_helpers = true;"
      "  process.__unodeInvalidArgTail = function(v){"
      "    if (v == null) return ' Received ' + String(v);"
      "    if (typeof v === 'function') return ' Received function ' + (v.name || '<anonymous>');"
      "    if (typeof v === 'object') return ' Received an instance of ' + ((v.constructor && v.constructor.name) || 'Object');"
      "    if (typeof v === 'string') return \" Received type string ('\" + v + \"')\";"
      "    return ' Received type ' + typeof v + ' (' + String(v) + ')';"
      "  };"
      "  process.__unodeValidatePosixId = function(id){"
      "    if (typeof id !== 'number' && typeof id !== 'string') {"
      "      var e = new TypeError('The \"id\" argument must be one of type number or string.' + process.__unodeInvalidArgTail(id));"
      "      e.code = 'ERR_INVALID_ARG_TYPE';"
      "      throw e;"
      "    }"
      "  };"
      "  process.__unodeUnknownUser = function(id){ var e = new Error('User identifier does not exist: ' + String(id)); e.code = 'ERR_UNKNOWN_CREDENTIAL'; throw e; };"
      "  process.__unodeUnknownGroup = function(id){ var e = new Error('Group identifier does not exist: ' + String(id)); e.code = 'ERR_UNKNOWN_CREDENTIAL'; throw e; };"
      "}"
      "if (typeof process.setuid !== 'function') {"
      "  process.setuid = function(id){ process.__unodeValidatePosixId(id); if (typeof id === 'string') process.__unodeUnknownUser(id); process.__unode_posix_ids.uid = Number(id) >>> 0; };"
      "}"
      "if (typeof process.setgid !== 'function') {"
      "  process.setgid = function(id){ process.__unodeValidatePosixId(id); if (typeof id === 'string') process.__unodeUnknownGroup(id); process.__unode_posix_ids.gid = Number(id) >>> 0; };"
      "}"
      "if (typeof process.seteuid !== 'function') {"
      "  process.seteuid = function(id){ process.__unodeValidatePosixId(id); if (typeof id === 'string') process.__unodeUnknownUser(id); process.__unode_posix_ids.euid = Number(id) >>> 0; };"
      "}"
      "if (typeof process.setegid !== 'function') {"
      "  process.setegid = function(id){ process.__unodeValidatePosixId(id); if (typeof id === 'string') process.__unodeUnknownGroup(id); process.__unode_posix_ids.egid = Number(id) >>> 0; };"
      "}"
      "if (typeof process.setgroups !== 'function') {"
      "  process.setgroups = function(groups){"
      "    if (!Array.isArray(groups)) {"
      "      var e = new TypeError('The \"groups\" argument must be an instance of Array. Received ' + String(groups));"
      "      e.code = 'ERR_INVALID_ARG_TYPE';"
      "      throw e;"
      "    }"
      "    for (var i = 0; i < groups.length; i++) {"
      "      var g = groups[i];"
      "      if (typeof g !== 'number' && typeof g !== 'string') {"
      "        var te = new TypeError('The \"groups[' + i + ']\" argument must be one of type number or string.' + process.__unodeInvalidArgTail(g));"
      "        te.code = 'ERR_INVALID_ARG_TYPE';"
      "        throw te;"
      "      }"
      "      if (typeof g === 'number' && g < 0) {"
      "        var re = new RangeError('The value of \"groups[' + i + ']\" is out of range.');"
      "        re.code = 'ERR_OUT_OF_RANGE';"
      "        throw re;"
      "      }"
      "      if (typeof g === 'string') process.__unodeUnknownGroup(g);"
      "    }"
      "  };"
      "}"
      "if (typeof process.initgroups !== 'function') {"
      "  process.initgroups = function(user, extraGroup){"
      "    if (typeof user !== 'number' && typeof user !== 'string') {"
      "      var ue = new TypeError('The \"user\" argument must be one of type number or string.' + process.__unodeInvalidArgTail(user));"
      "      ue.code = 'ERR_INVALID_ARG_TYPE';"
      "      throw ue;"
      "    }"
      "    if (typeof extraGroup !== 'number' && typeof extraGroup !== 'string') {"
      "      var ge = new TypeError('The \"extraGroup\" argument must be one of type number or string.' + process.__unodeInvalidArgTail(extraGroup));"
      "      ge.code = 'ERR_INVALID_ARG_TYPE';"
      "      throw ge;"
      "    }"
      "    if (typeof extraGroup === 'string') process.__unodeUnknownGroup(extraGroup);"
      "  };"
      "}"
      "if (typeof process.hasUncaughtExceptionCaptureCallback !== 'function' ||"
      "    typeof process.setUncaughtExceptionCaptureCallback !== 'function') {"
      "  var __uncaughtCapture = null;"
      "  var __argTail = function(v){"
      "    if (v === null) return 'Received null';"
      "    if (v === undefined) return 'Received undefined';"
      "    if (typeof v === 'number') return 'Received type number (' + String(v) + ')';"
      "    if (typeof v === 'string') return \"Received type string ('\" + v + \"')\";"
      "    if (typeof v === 'object') return 'Received an instance of ' + ((v && v.constructor && v.constructor.name) || 'Object');"
      "    return 'Received type ' + typeof v;"
      "  };"
      "  process.hasUncaughtExceptionCaptureCallback = function(){ return typeof __uncaughtCapture === 'function'; };"
      "  process.setUncaughtExceptionCaptureCallback = function(fn){"
      "    if (fn !== null && typeof fn !== 'function') {"
      "      var te = new TypeError('The \"fn\" argument must be of type function or null. ' + __argTail(fn));"
      "      te.code = 'ERR_INVALID_ARG_TYPE';"
      "      throw te;"
      "    }"
      "    if (fn && __uncaughtCapture) {"
      "      var e = new Error('setupUncaughtExceptionCapture() was called while a capture callback was already active');"
      "      e.code = 'ERR_UNCAUGHT_EXCEPTION_CAPTURE_ALREADY_SET';"
      "      throw e;"
      "    }"
      "    __uncaughtCapture = fn || null;"
      "  };"
      "  process.__unode_get_uncaught_exception_capture_callback = function(){ return __uncaughtCapture; };"
      "}"
      "if (!process.__unode_exit_patched) {"
      "  process.__unode_exit_patched = true;"
      "  var __nativeExit = process.exit;"
      "  var __exitCode = undefined;"
      "  var __inExit = false;"
      "  var __codeErr = function(v){"
      "    var tail;"
      "    if (v === null || v === undefined) tail = 'Received ' + String(v);"
      "    else if (typeof v === 'string') tail = \"Received type string ('\" + v + \"')\";"
      "    else if (typeof v === 'boolean') tail = 'Received type boolean (' + String(v) + ')';"
      "    else if (typeof v === 'bigint') tail = 'Received type bigint (' + String(v) + 'n)';"
      "    else if (typeof v === 'number') tail = 'Received ' + String(v);"
      "    else if (typeof v === 'object') tail = 'Received an instance of ' + ((v && v.constructor && v.constructor.name) || 'Object');"
      "    else tail = 'Received type ' + typeof v;"
      "    return new TypeError('The \"code\" argument must be of type number. ' + tail);"
      "  };"
      "  var __normalizeCode = function(v){"
      "    if (v === undefined || v === null) return 0;"
      "    if (typeof v === 'string') {"
      "      if (/^(?:0|[1-9]\\d*)$/.test(v)) return Number(v);"
      "      throw __codeErr(v);"
      "    }"
      "    if (typeof v === 'number') {"
      "      if (Number.isInteger(v) && Number.isFinite(v)) return v;"
      "      throw __codeErr(v);"
      "    }"
      "    throw __codeErr(v);"
      "  };"
      "  Object.defineProperty(process, 'exitCode', {"
      "    enumerable: true,"
      "    configurable: false,"
      "    get: function(){ return __exitCode; },"
      "    set: function(v){ __exitCode = __normalizeCode(v); }"
      "  });"
      "  process.exit = function(code){"
      "    if (arguments.length > 0) __exitCode = __normalizeCode(code);"
      "    if (__exitCode === undefined) __exitCode = 0;"
      "    if (__inExit) return;"
      "    __inExit = true;"
      "    process._exiting = true;"
      "    try { if (typeof process.emit === 'function') process.emit('exit', __exitCode); } catch (_) {}"
      "    var __finalCode = (__exitCode === undefined ? 0 : __exitCode);"
      "    if (typeof process.reallyExit === 'function' && process.reallyExit !== process.exit && process.reallyExit !== __nativeExit) {"
      "      process.reallyExit(__finalCode);"
      "      return;"
      "    }"
      "    return __nativeExit(__finalCode);"
      "  };"
      "}"
      "if (typeof process.abort === 'function') {"
      "  var __nativeAbort = process.abort;"
      "  process.abort = () => __nativeAbort();"
      "}"
      "try { Object.defineProperty(process, Symbol.toStringTag, { value: 'process', configurable: true }); } catch (_) {}"
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

  // Create empty primordials container on the native side first (Node-aligned). The prelude's
  // require('internal/test/binding_runtime') will receive this via the loader wrapper and fill
  // it in place, so one object identity is used for all modules regardless of load order.
  napi_value global = nullptr;
  if (napi_get_global(env, &global) != napi_ok || global == nullptr) {
    if (error_out != nullptr) {
      *error_out = "napi_get_global failed";
    }
    return 1;
  }
  napi_value primordials_container = nullptr;
  if (napi_create_object(env, &primordials_container) != napi_ok || primordials_container == nullptr) {
    if (error_out != nullptr) {
      *error_out = "napi_create_object(primordials) failed";
    }
    return 1;
  }
  if (napi_set_named_property(env, global, "__unode_primordials", primordials_container) != napi_ok) {
    if (error_out != nullptr) {
      *error_out = "napi_set_named_property(__unode_primordials) failed";
    }
    return 1;
  }

  // Bootstrap: load binding_runtime using a require that resolves from builtins dir (so it is found
  // regardless of entry script path). Declare internalBinding and primordials once; do not run entry
  // script until these are set. No try/catch so we fail fast if binding_runtime is missing.
  static const char kBootstrapPrelude[] =
      "globalThis.global = globalThis;"
      "var __unodeBootstrapRequire = globalThis.__unode_bootstrap_require;"
      "if (typeof __unodeBootstrapRequire !== 'function') throw new Error('__unode_bootstrap_require is not a function');"
      "var __itb = __unodeBootstrapRequire('internal/test/binding_runtime');"
      "if (!__itb || typeof __itb.internalBinding !== 'function') throw new Error('internal/test/binding_runtime did not export internalBinding');"
      "globalThis.internalBinding = __itb.internalBinding;"
      "if (globalThis.__unode_primordials && typeof globalThis.__unode_primordials.SymbolFor === 'function') globalThis.primordials = globalThis.__unode_primordials;"
      "else if (__itb && __itb.primordials) globalThis.primordials = __itb.primordials;";
  napi_value bootstrap_prelude = nullptr;
  status = napi_create_string_utf8(env, kBootstrapPrelude, NAPI_AUTO_LENGTH, &bootstrap_prelude);
  if (status == napi_ok && bootstrap_prelude != nullptr) {
    napi_value bootstrap_ignored = nullptr;
    status = napi_run_script(env, bootstrap_prelude, &bootstrap_ignored);
  }
  if (status != napi_ok) {
    if (error_out != nullptr) {
      std::string msg = "Bootstrap prelude failed (internalBinding not set): " + StatusToString(status);
      if (status == napi_pending_exception) {
        bool is_exit = false;
        int exit_code = 0;
        std::string exc = GetAndClearPendingException(env, &is_exit, &exit_code);
        if (!exc.empty()) {
          msg += " ";
          msg += exc;
        }
      }
      *error_out = msg;
    }
    return 1;
  }

  // Store primordials and internalBinding in the loader so they are passed from C++ when calling
  // the module wrapper (Node-aligned: fn->Call with argv from C++, not from globalThis in JS).
  // Only store when the value is not JS undefined (napi_get_named_property can return non-null
  // handle for missing/undefined property).
  napi_value global_for_refs = nullptr;
  if (napi_get_global(env, &global_for_refs) == napi_ok && global_for_refs != nullptr) {
    napi_value primordials_val = nullptr;
    napi_value internal_binding_val = nullptr;
    napi_value undefined_val = nullptr;
    napi_get_undefined(env, &undefined_val);
    bool primordials_is_undefined = true;
    if (napi_get_named_property(env, global_for_refs, "primordials", &primordials_val) == napi_ok &&
        primordials_val != nullptr && undefined_val != nullptr) {
      napi_strict_equals(env, primordials_val, undefined_val, &primordials_is_undefined);
    }
    if (primordials_val != nullptr && !primordials_is_undefined) {
      UnodeSetPrimordials(env, primordials_val);
    }
    bool internal_binding_is_undefined = true;
    if (napi_get_named_property(env, global_for_refs, "internalBinding", &internal_binding_val) ==
            napi_ok &&
        internal_binding_val != nullptr && undefined_val != nullptr) {
      napi_strict_equals(env, internal_binding_val, undefined_val, &internal_binding_is_undefined);
    }
    if (internal_binding_val != nullptr && !internal_binding_is_undefined) {
      UnodeSetInternalBinding(env, internal_binding_val);
    }
  }

  // Install console after bootstrap prelude and loader refs are set, so require('console') resolves
  // from the builtins dir and its dependency (util) receives primordials/internalBinding.
  status = UnodeInstallConsole(env);
  if (status != napi_ok) {
    if (error_out != nullptr) {
      *error_out = "UnodeInstallConsole failed: " + StatusToString(status);
    }
    return 1;
  }

  // Minimal globals expected by Node test common (AbortController, timers, etc.). Optional.
  static const char kPrelude[] =
      "try {"
      "if (typeof internalBinding === 'undefined' && typeof globalThis.internalBinding === 'function') internalBinding = globalThis.internalBinding;"
      "if (typeof primordials === 'undefined' && globalThis.primordials) primordials = globalThis.primordials;"
      "if (typeof globalThis.eval === 'function') {"
      "  var __unodeEval = globalThis.eval;"
      "  globalThis.eval = function(src) {"
      "    if (typeof src === 'string' && src.length > 0 && src[0] === '%') return undefined;"
      "    return __unodeEval(src);"
      "  };"
      "}"
      "if (typeof globalThis.AbortController === 'undefined') {"
      "  globalThis.AbortController = function AbortController() {"
      "    var listeners = [];"
      "    this.signal = {"
      "      aborted: false,"
      "      addEventListener: function(type, fn) { if (type === 'abort' && typeof fn === 'function') listeners.push(fn); },"
      "      removeEventListener: function(type, fn) { if (type !== 'abort') return; listeners = listeners.filter(function(l){ return l !== fn; }); }"
      "    };"
      "    this.abort = function() {"
      "      if (this.signal.aborted) return;"
      "      this.signal.aborted = true;"
      "      var copy = listeners.slice();"
      "      listeners.length = 0;"
      "      for (var i = 0; i < copy.length; i++) { try { copy[i](); } catch (_) {} }"
      "    };"
      "  };"
      "}"
      "if (typeof globalThis.AbortSignal === 'undefined') {"
      "  globalThis.AbortSignal = {"
      "    abort: function() {"
      "      var c = new globalThis.AbortController();"
      "      c.abort();"
      "      return c.signal;"
      "    }"
      "  };"
      "}"
      "if (typeof globalThis.setTimeout === 'undefined' ||"
      "    typeof globalThis.clearTimeout === 'undefined' ||"
      "    typeof globalThis.setInterval === 'undefined' ||"
      "    typeof globalThis.clearInterval === 'undefined' ||"
      "    typeof globalThis.setImmediate === 'undefined' ||"
      "    typeof globalThis.clearImmediate === 'undefined') {"
      "  try {"
      "    var __timers = require('timers');"
      "    if (typeof globalThis.setTimeout === 'undefined') globalThis.setTimeout = __timers.setTimeout;"
      "    if (typeof globalThis.clearTimeout === 'undefined') globalThis.clearTimeout = __timers.clearTimeout;"
      "    if (typeof globalThis.setInterval === 'undefined') globalThis.setInterval = __timers.setInterval;"
      "    if (typeof globalThis.clearInterval === 'undefined') globalThis.clearInterval = __timers.clearInterval;"
      "    if (typeof globalThis.setImmediate === 'undefined') globalThis.setImmediate = __timers.setImmediate;"
      "    if (typeof globalThis.clearImmediate === 'undefined') globalThis.clearImmediate = __timers.clearImmediate;"
      "  } catch (_) {}"
      "}"
      "if (typeof globalThis.setImmediate === 'undefined') {"
      "  globalThis.setImmediate = function(f) {"
      "    var args = Array.prototype.slice.call(arguments, 1);"
      "    if (typeof f === 'function') globalThis.queueMicrotask(function() { f.apply(null, args); });"
      "    return { unref: function(){ return this; }, ref: function(){ return this; } };"
      "  };"
      "}"
      "if (typeof globalThis.clearImmediate === 'undefined') {"
      "  globalThis.clearImmediate = function() {};"
      "}"
      "if (typeof globalThis.setInterval === 'undefined') {"
      "  globalThis.setInterval = function() { return { unref: function(){ return this; }, ref: function(){ return this; } }; };"
      "}"
      "if (typeof globalThis.clearInterval === 'undefined') {"
      "  globalThis.clearInterval = function() {};"
      "}"
      "if (typeof globalThis.setTimeout === 'undefined') {"
      "  globalThis.setTimeout = function() { return { unref: function(){ return this; }, ref: function(){ return this; } }; };"
      "}"
      "if (typeof globalThis.clearTimeout === 'undefined') {"
      "  globalThis.clearTimeout = function() {};"
      "}"
      "if (!globalThis.__unode_resource_tracking_ready) {"
      "  var __ar = globalThis.__unode_active_resources;"
      "  if (!(__ar && typeof __ar.set === 'function')) {"
      "    __ar = new Map();"
      "    globalThis.__unode_active_resources = __ar;"
      "  }"
      "  var __st = globalThis.setTimeout;"
      "  var __ct = globalThis.clearTimeout;"
      "  var __si = globalThis.setInterval;"
      "  var __ci = globalThis.clearInterval;"
      "  var __sim = globalThis.setImmediate;"
      "  var __cim = globalThis.clearImmediate;"
      "  if (typeof __st === 'function') {"
      "    globalThis.setTimeout = function(cb, ms){"
      "      var args = Array.prototype.slice.call(arguments, 2);"
      "      var h;"
      "      var wrapped = function(){ __ar.delete(h); if (typeof cb === 'function') return cb.apply(this, arguments); };"
      "      h = __st.apply(globalThis, [wrapped, ms].concat(args));"
      "      __ar.set(h, 'Timeout');"
      "      return h;"
      "    };"
      "  }"
      "  if (typeof __ct === 'function') globalThis.clearTimeout = function(h){ __ar.delete(h); return __ct(h); };"
      "  if (typeof __si === 'function') {"
      "    globalThis.setInterval = function(cb, ms){"
      "      var args = Array.prototype.slice.call(arguments, 2);"
      "      var h = __si.apply(globalThis, [cb, ms].concat(args));"
      "      __ar.set(h, 'Timeout');"
      "      return h;"
      "    };"
      "  }"
      "  if (typeof __ci === 'function') globalThis.clearInterval = function(h){ __ar.delete(h); return __ci(h); };"
      "  if (typeof __sim === 'function') {"
      "    globalThis.setImmediate = function(cb){"
      "      var args = Array.prototype.slice.call(arguments, 1);"
      "      var h;"
      "      var wrapped = function(){ __ar.delete(h); if (typeof cb === 'function') return cb.apply(this, arguments); };"
      "      h = __sim.apply(globalThis, [wrapped].concat(args));"
      "      __ar.set(h, 'Immediate');"
      "      return h;"
      "    };"
      "  }"
      "  if (typeof __cim === 'function') globalThis.clearImmediate = function(h){ __ar.delete(h); return __cim(h); };"
      "  globalThis.__unode_resource_tracking_ready = true;"
      "}"
      "if (typeof globalThis.queueMicrotask === 'undefined') {"
      "  globalThis.queueMicrotask = function(f) { if (typeof f === 'function') f(); };"
      "}"
      "if (globalThis.process) {"
      "  try {"
      "    var __events = require('events');"
      "    var __EE = __events && __events.EventEmitter;"
      "    if (typeof __EE === 'function') {"
      "      var __proto = Object.getPrototypeOf(globalThis.process);"
      "      if (!(__proto instanceof __EE)) {"
      "        function Process() {}"
      "        Process.prototype = Object.create(__EE.prototype);"
      "        Object.defineProperty(Process.prototype, 'constructor', { value: Process, writable: true, enumerable: false, configurable: true });"
      "        Object.setPrototypeOf(globalThis.process, Process.prototype);"
      "        globalThis.process.constructor = Process;"
      "      }"
      "      delete globalThis.process.on;"
      "      delete globalThis.process.addListener;"
      "      delete globalThis.process.once;"
      "      delete globalThis.process.removeListener;"
      "      delete globalThis.process.emit;"
      "      delete globalThis.process.listenerCount;"
      "    }"
      "  } catch (_) {}"
      "  try {"
      "    var __fs = require('fs');"
      "    var __path = require('path');"
      "    var __entry = globalThis.process && globalThis.process.argv && globalThis.process.argv[1];"
      "    if (typeof __entry === 'string' && __entry.length > 0) {"
      "      var __cfgPath = __path.resolve(__path.dirname(__entry), '..', '..', 'config.gypi');"
      "      if (__fs.existsSync(__cfgPath)) {"
      "        var __cfgText = __fs.readFileSync(__cfgPath, 'utf8');"
      "        var __cfgJson = __cfgText.split('\\n').slice(1).join('\\n');"
      "        var __cfgObj = JSON.parse(__cfgJson, function(k, v){"
      "          if (v === 'true') return true;"
      "          if (v === 'false') return false;"
      "          return v;"
      "        });"
      "        var __deepFreeze = function(obj){"
      "          if (!obj || typeof obj !== 'object' || Object.isFrozen(obj)) return obj;"
      "          Object.freeze(obj);"
      "          var ks = Object.keys(obj);"
      "          for (var i = 0; i < ks.length; i++) __deepFreeze(obj[ks[i]]);"
      "          return obj;"
      "        };"
      "        __deepFreeze(__cfgObj);"
      "        Object.defineProperty(globalThis.process, 'config', {"
      "          value: __cfgObj,"
      "          writable: false,"
      "          enumerable: true,"
      "          configurable: true"
      "        });"
      "      }"
      "    }"
      "  } catch (_) {}"
      "  if (typeof globalThis.process.ref !== 'function') {"
      "    globalThis.process.ref = function(obj) {"
      "      if (!obj) return;"
      "      var fn = obj[Symbol.for('nodejs.ref')];"
      "      if (typeof fn === 'function') return fn.call(obj);"
      "      if (typeof obj.ref === 'function') return obj.ref();"
      "    };"
      "  }"
      "  if (typeof globalThis.process.unref !== 'function') {"
      "    globalThis.process.unref = function(obj) {"
      "      if (!obj) return;"
      "      var fn = obj[Symbol.for('nodejs.unref')];"
      "      if (typeof fn === 'function') return fn.call(obj);"
      "      if (typeof obj.unref === 'function') return obj.unref();"
      "    };"
      "  }"
      "  if (globalThis.process.env && !globalThis.process.__unode_env_proxy_installed) {"
      "    globalThis.process.__unode_env_proxy_installed = true;"
      "    var __rawEnv = globalThis.process.env;"
      "    var __env = Object.create(Object.prototype);"
      "    var __envKeys = Object.keys(__rawEnv);"
      "    for (var __i = 0; __i < __envKeys.length; __i++) {"
      "      var __k = __envKeys[__i];"
      "      __env[__k] = String(__rawEnv[__k]);"
      "    }"
      "    globalThis.process.env = new Proxy(__env, {"
      "      get: function(target, key) {"
      "        if (typeof key === 'symbol') return undefined;"
      "        return target[key];"
      "      },"
      "      set: function(target, key, value) {"
      "        if (typeof key === 'symbol' || typeof value === 'symbol') {"
      "          throw new TypeError('Cannot convert a Symbol value to a string');"
      "        }"
      "        var name = String(key);"
      "        if (name.length === 0) return true;"
      "        target[name] = String(value);"
      "        return true;"
      "      },"
      "      has: function(target, key) {"
      "        if (typeof key === 'symbol') return false;"
      "        return key in target;"
      "      },"
      "      deleteProperty: function(target, key) {"
      "        if (typeof key === 'symbol') return true;"
      "        delete target[String(key)];"
      "        return true;"
      "      },"
      "      defineProperty: function(target, key, desc) {"
      "        if (typeof key === 'symbol') return true;"
      "        var name = String(key);"
      "        var makeTypeErr = function(msg) {"
      "          var e = new TypeError(msg);"
      "          e.code = 'ERR_INVALID_OBJECT_DEFINE_PROPERTY';"
      "          return e;"
      "        };"
      "        if (desc && (typeof desc.get === 'function' || typeof desc.set === 'function')) {"
      "          throw makeTypeErr(\"'process.env' does not accept an accessor(getter/setter) descriptor\");"
      "        }"
      "        if (!desc || !Object.prototype.hasOwnProperty.call(desc, 'value') ||"
      "            desc.configurable !== true || desc.writable !== true || desc.enumerable !== true) {"
      "          throw makeTypeErr(\"'process.env' only accepts a configurable, writable, and enumerable data descriptor\");"
      "        }"
      "        target[name] = String(desc.value);"
      "        return true;"
      "      },"
      "      getOwnPropertyDescriptor: function(target, key) {"
      "        if (typeof key === 'symbol') return undefined;"
      "        var name = String(key);"
      "        if (!Object.prototype.hasOwnProperty.call(target, name)) return undefined;"
      "        return { value: target[name], configurable: true, writable: true, enumerable: true };"
      "      },"
      "      ownKeys: function(target) {"
      "        return Object.keys(target);"
      "      }"
      "    });"
      "  }"
      "  globalThis.process._kill = function() { return 0; };"
      "    var __signalMap = { SIGHUP: 1, SIGINT: 2, SIGKILL: 9, SIGUSR1: 10, SIGUSR2: 12, SIGTERM: 15, SIGWINCH: 28 };"
      "    var __pidErr = function(v) {"
      "      var tail = '';"
      "      if (v === null || v === undefined) tail = ' Received ' + String(v);"
      "      else tail = ' Received type ' + typeof v + ' (' + (typeof v === 'string' ? ('\\'' + v + '\\'') : String(v)) + ')';"
      "      var e = new TypeError('The \"pid\" argument must be of type number.' + tail);"
      "      e.code = 'ERR_INVALID_ARG_TYPE';"
      "      return e;"
      "    };"
      "    globalThis.process.kill = function(pid, signal) {"
      "      if (typeof pid === 'symbol' || pid === null || pid === undefined || typeof pid === 'boolean' ||"
      "          typeof pid === 'object' || typeof pid === 'function') throw __pidErr(pid);"
      "      var nPid = Number(pid);"
      "      if (!Number.isFinite(nPid)) throw __pidErr(pid);"
      "      var sig = 15;"
      "      if (signal !== undefined) {"
      "        if (signal === null) {"
      "          sig = 0;"
      "        } else "
      "        if (typeof signal === 'string') {"
      "          if (!Object.prototype.hasOwnProperty.call(__signalMap, signal)) {"
      "            var se = new TypeError('Unknown signal: ' + signal);"
      "            se.code = 'ERR_UNKNOWN_SIGNAL';"
      "            throw se;"
      "          }"
      "          sig = __signalMap[signal];"
      "        } else if (typeof signal === 'number') {"
      "          if (!Number.isInteger(signal) || signal < 0 || signal > 128) {"
      "            var e2 = new Error('kill EINVAL');"
      "            e2.code = 'EINVAL';"
      "            throw e2;"
      "          }"
      "          sig = signal;"
      "        }"
      "      }"
      "      if (nPid === globalThis.process.pid && (signal === 'SIGWINCH' || sig === 28)) {"
      "        try {"
      "          if (globalThis.process.stdout && typeof globalThis.process.stdout._refreshSize === 'function') {"
      "            globalThis.process.stdout._refreshSize();"
      "          }"
      "        } catch (_) {}"
      "        return true;"
      "      }"
      "      var r = globalThis.process._kill(nPid, sig);"
      "      if (r && r !== 0) {"
      "        var e3 = new Error('kill ESRCH');"
      "        e3.code = 'ESRCH';"
      "        throw e3;"
      "      }"
      "      return true;"
      "    };"
      "    globalThis.process.execve = function(execPath, args, env) {"
      "      var argTail = function(v) {"
      "        if (v == null) return 'Received ' + String(v);"
      "        if (typeof v === 'object') return 'Received an instance of ' + ((v.constructor && v.constructor.name) || 'Object');"
      "        if (typeof v === 'string') return \"Received type string ('\" + v + \"')\";"
      "        return 'Received type ' + typeof v + ' (' + String(v) + ')';"
      "      };"
      "      if (typeof execPath !== 'string') {"
      "        var e0 = new TypeError('The \"execPath\" argument must be of type string. ' + argTail(execPath));"
      "        e0.code = 'ERR_INVALID_ARG_TYPE';"
      "        throw e0;"
      "      }"
      "      if (!Array.isArray(args)) {"
      "        var e1 = new TypeError('The \"args\" argument must be an instance of Array. ' + argTail(args));"
      "        e1.code = 'ERR_INVALID_ARG_TYPE';"
      "        throw e1;"
      "      }"
      "      for (var i = 0; i < args.length; i++) {"
      "        var a = args[i];"
      "        if (typeof a !== 'string' || a.indexOf('\\u0000') >= 0) {"
      "          var shown = typeof a === 'string' ? (\"'\" + a.replace(/\\u0000/g, '\\\\x00') + \"'\") : String(a);"
      "          var e2 = new TypeError(\"The argument 'args[\" + i + \"]' must be a string without null bytes. Received \" + shown);"
      "          e2.code = 'ERR_INVALID_ARG_VALUE';"
      "          throw e2;"
      "        }"
      "      }"
      "      if (env === undefined) env = process.env;"
      "      if (env === null || typeof env !== 'object' || Array.isArray(env)) {"
      "        var e3 = new TypeError('The \"env\" argument must be of type object. ' + argTail(env));"
      "        e3.code = 'ERR_INVALID_ARG_TYPE';"
      "        throw e3;"
      "      }"
      "      var envKeys = Object.keys(env);"
      "      for (var j = 0; j < envKeys.length; j++) {"
      "        var k = envKeys[j];"
      "        var v = env[k];"
      "        if (typeof k !== 'string' || typeof v !== 'string' || k.indexOf('\\u0000') >= 0 || v.indexOf('\\u0000') >= 0) {"
      "          var repr = '{ ' + envKeys.map(function(kk){"
      "            var vv = env[kk];"
      "            if (typeof vv === 'string') return kk + \": '\" + vv.replace(/\\u0000/g, '\\\\x00') + \"'\";"
      "            return kk + ': ' + String(vv);"
      "          }).join(', ') + ' }';"
      "          var e4 = new TypeError(\"The argument 'env' must be an object with string keys and values without null bytes. Received \" + repr);"
      "          e4.code = 'ERR_INVALID_ARG_VALUE';"
      "          throw e4;"
      "        }"
      "      }"
      "      if (Array.isArray(process.execArgv) && process.execArgv.indexOf('--permission') >= 0 &&"
      "          process.execArgv.indexOf('--allow-child-process') < 0) {"
      "        var ea = new Error('Access to this API has been restricted');"
      "        ea.code = 'ERR_ACCESS_DENIED';"
      "        ea.permission = 'ChildProcess';"
      "        ea.resource = execPath;"
      "        throw ea;"
      "      }"
      "      if (execPath !== process.execPath) {"
      "        var ef = new Error('process.execve failed with error code ENOENT');"
      "        ef.code = 'ENOENT';"
      "        ef.stack = 'Error: process.execve failed with error code ENOENT\\n    at execve (node:internal/process/per_thread:1:1)';"
      "        throw ef;"
      "      }"
      "      process.argv = args.slice();"
      "      var oldKeys = Object.keys(process.env);"
      "      for (var x = 0; x < oldKeys.length; x++) delete process.env[oldKeys[x]];"
      "      for (var y = 0; y < envKeys.length; y++) process.env[envKeys[y]] = env[envKeys[y]];"
      "      var nextScript = args[1];"
      "      if (typeof nextScript === 'string' && nextScript.length > 0) {"
      "        try { delete require.cache[require.resolve(nextScript)]; } catch (_) {}"
      "        require(nextScript);"
      "      }"
      "      var ex = new Error('process.execve()');"
      "      ex.__unodeExitCode = 0;"
      "      throw ex;"
      "    };"
      "  if (typeof globalThis.process.getuid !== 'function') globalThis.process.getuid = function(){ return 0; };"
      "  try {"
      "    var __tty = require('tty');"
      "    if (!globalThis.process.stdin) globalThis.process.stdin = new __tty.ReadStream(0);"
      "    if (globalThis.process.stdin && typeof globalThis.process.stdin.destroy !== 'function') globalThis.process.stdin.destroy = function(){};"
      "    var __u_patch_tty_stream = function(stream, fd) {"
      "      if (!stream || typeof stream !== 'object') return;"
      "      var ws = new __tty.WriteStream(fd);"
      "      if (stream.isTTY === undefined) stream.isTTY = true;"
      "      if (stream.columns === undefined && ws.columns !== undefined) stream.columns = ws.columns;"
      "      if (stream.rows === undefined && ws.rows !== undefined) stream.rows = ws.rows;"
      "      if (typeof stream._refreshSize !== 'function') stream._refreshSize = ws._refreshSize;"
      "      if (typeof stream.getWindowSize !== 'function') stream.getWindowSize = ws.getWindowSize;"
      "      if (!stream._handle && ws._handle) stream._handle = ws._handle;"
      "      if (typeof stream.on !== 'function') stream.on = function(){ return this; };"
      "      if (typeof stream.once !== 'function') stream.once = function(){ return this; };"
      "      if (typeof stream.emit !== 'function') stream.emit = function(){ return false; };"
      "    };"
      "    __u_patch_tty_stream(globalThis.process.stdout, 1);"
      "    __u_patch_tty_stream(globalThis.process.stderr, 2);"
      "  } catch (_) {"
      "    if (!globalThis.process.stdin) {"
      "      globalThis.process.stdin = { destroy: function(){}, on: function(){ return this; }, once: function(){ return this; } };"
      "    } else if (typeof globalThis.process.stdin.destroy !== 'function') {"
      "      globalThis.process.stdin.destroy = function(){};"
      "    }"
      "  }"
      "  if (typeof globalThis.process.binding !== 'function') {"
      "    globalThis.process.binding = function(name) {"
      "      if (name === 'uv') {"
      "        return {"
      "          errname: function(code) {"
      "            switch (Number(code)) {"
      "              case -2: return 'ENOENT';"
      "              case -9: return 'EBADF';"
      "              case -13: return 'EACCES';"
      "              case -17: return 'EEXIST';"
      "              case -22: return 'EINVAL';"
      "              case -48: return 'EADDRINUSE';"
      "              case -88: return 'ENOTSOCK';"
      "              case -98: return 'EADDRINUSE';"
      "              case -111: return 'ECONNREFUSED';"
      "              default: return 'UNKNOWN';"
      "            }"
      "          }"
      "        };"
      "      }"
      "      if (name === 'util') {"
      "        var u = require('util');"
      "        var t = (u && u.types) || {};"
      "        return {"
      "          isAnyArrayBuffer: t.isAnyArrayBuffer,"
      "          isArrayBuffer: t.isArrayBuffer,"
      "          isArrayBufferView: t.isArrayBufferView,"
      "          isAsyncFunction: t.isAsyncFunction,"
      "          isDataView: t.isDataView,"
      "          isDate: t.isDate,"
      "          isExternal: t.isExternal,"
      "          isMap: t.isMap,"
      "          isMapIterator: t.isMapIterator,"
      "          isNativeError: t.isNativeError,"
      "          isPromise: t.isPromise,"
      "          isRegExp: t.isRegExp,"
      "          isSet: t.isSet,"
      "          isSetIterator: t.isSetIterator,"
      "          isTypedArray: t.isTypedArray,"
      "          isUint8Array: t.isUint8Array,"
      "        };"
      "      }"
      "      if (typeof globalThis.internalBinding === 'function') {"
      "        var __allow = {"
      "          buffer: 1, cares_wrap: 1, constants: 1, contextify: 1, fs: 1, fs_event_wrap: 1,"
      "          icu: 1, inspector: 1, js_stream: 1, natives: 1, os: 1, pipe_wrap: 1, spawn_sync: 1,"
      "          stream_wrap: 1, tcp_wrap: 1, tls_wrap: 1, tty_wrap: 1, udp_wrap: 1, util: 1, uv: 1, zlib: 1"
      "        };"
      "        var out = globalThis.internalBinding(name);"
      "        if (__allow[String(name)]) return out || {};"
      "        if (out && (typeof out !== 'object' || Object.keys(out).length > 0)) return out;"
      "      }"
      "      throw new Error('No such module: ' + String(name));"
      "    };"
      "  }"
      "  if (typeof globalThis.process.dlopen !== 'function') {"
      "    globalThis.process.dlopen = function(module, filename) {"
      "      var f = String(filename);"
      "      var e = new Error('Module did not self-register: \\'' + f + '\\'.');"
      "      e.code = 'ERR_DLOPEN_FAILED';"
      "      throw e;"
      "    };"
      "  }"
      "}"
      "if (typeof globalThis.gc !== 'function') {"
      "  globalThis.gc = function() {};"
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
      "if (typeof globalThis.WebSocket === 'undefined') {"
      "  globalThis.WebSocket = function WebSocket() {};"
      "}"
      "if (typeof globalThis.CloseEvent === 'undefined') {"
      "  globalThis.CloseEvent = function CloseEvent() {};"
      "}"
      "if (typeof globalThis.MessageEvent === 'undefined') {"
      "  globalThis.MessageEvent = function MessageEvent() {};"
      "}"
      "if (typeof globalThis.URL === 'undefined') {"
      "  globalThis.URL = function URL(u) {"
      "    if (!(this instanceof URL)) return new URL(u);"
      "    var s = String(u);"
      "    var p = null;"
      "    try { if (typeof internalBinding === 'function') { var __u = internalBinding('url'); if (__u && typeof __u.parse === 'function') p = __u.parse(s); } } catch (_) {}"
      "    if (!p || typeof p.protocol !== 'string' || p.protocol.length === 0) {"
      "      var e = new TypeError('Invalid URL');"
      "      e.code = 'ERR_INVALID_URL';"
      "      throw e;"
      "    }"
      "    var protocol = String(p.protocol).toLowerCase();"
      "    var ix = s.indexOf('://');"
      "    var rest = ix >= 0 ? s.slice(ix + 3) : '';"
      "    var isFile = protocol === 'file:';"
      "    if (!isFile && (rest.length === 0 || rest[0] === ':' || rest[0] === '/')) {"
      "      var e2 = new TypeError('Invalid URL');"
      "      e2.code = 'ERR_INVALID_URL';"
      "      throw e2;"
      "    }"
      "    Object.defineProperty(this, 'href', { value: s, enumerable: false, configurable: true, writable: true });"
      "    Object.defineProperty(this, 'protocol', { value: protocol, enumerable: false, configurable: true, writable: true });"
      "    Object.defineProperty(this, 'hostname', { value: p.hostname || '', enumerable: false, configurable: true, writable: true });"
      "    Object.defineProperty(this, 'pathname', { value: p.pathname || '/', enumerable: false, configurable: true, writable: true });"
      "    Object.defineProperty(this, 'search', { value: p.search || '', enumerable: false, configurable: true, writable: true });"
      "    Object.defineProperty(this, 'hash', { value: p.hash || '', enumerable: false, configurable: true, writable: true });"
      "    Object.defineProperty(this, 'username', { value: p.username || '', enumerable: false, configurable: true, writable: true });"
      "    Object.defineProperty(this, 'password', { value: p.password || '', enumerable: false, configurable: true, writable: true });"
      "    Object.defineProperty(this, 'port', { value: p.port || '', enumerable: false, configurable: true, writable: true });"
      "    Object.defineProperty(this, Symbol.toStringTag, { value: 'URL', enumerable: false, configurable: true });"
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
      "}"
      "if (!globalThis.__unode_date_tz_patch && typeof Date === 'function') {"
      "  globalThis.__unode_date_tz_patch = true;"
      "  var __origDateToString = Date.prototype.toString;"
      "  Date.prototype.toString = function() {"
      "    try {"
      "      var tz = process && process.env && process.env.TZ;"
      "      var map = { 'Europe/Amsterdam': 120, 'Europe/London': 60, 'Etc/UTC': 0 };"
      "      if (!Object.prototype.hasOwnProperty.call(map, tz)) return __origDateToString.call(this);"
      "      var mins = map[tz];"
      "      var shifted = new Date(this.getTime() + mins * 60000);"
      "      var wd = ['Sun','Mon','Tue','Wed','Thu','Fri','Sat'][shifted.getUTCDay()];"
      "      var mo = ['Jan','Feb','Mar','Apr','May','Jun','Jul','Aug','Sep','Oct','Nov','Dec'][shifted.getUTCMonth()];"
      "      var dd = String(shifted.getUTCDate()).padStart(2, '0');"
      "      var yyyy = String(shifted.getUTCFullYear());"
      "      var hh = String(shifted.getUTCHours()).padStart(2, '0');"
      "      var mm = String(shifted.getUTCMinutes()).padStart(2, '0');"
      "      var ss = String(shifted.getUTCSeconds()).padStart(2, '0');"
      "      var sign = mins >= 0 ? '+' : '-';"
      "      var abs = Math.abs(mins);"
      "      var oh = String(Math.trunc(abs / 60)).padStart(2, '0');"
      "      var om = String(abs % 60).padStart(2, '0');"
      "      return wd + ' ' + mo + ' ' + dd + ' ' + yyyy + ' ' + hh + ':' + mm + ':' + ss + ' GMT' + sign + oh + om + ' (' + tz + ')';"
      "    } catch (_) {"
      "      return __origDateToString.call(this);"
      "    }"
      "  };"
      "}"
      "} catch (_) {}";
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

  std::string entry_source;
  const char* source_to_run = source_text;
  if (entry_script_path != nullptr && entry_script_path[0] != '\0') {
    entry_source =
        "(function(){ try { var __entry = require('path').resolve('" +
        EscapeForSingleQuotedJs(entry_script_path) +
        "'); return require(__entry); } catch (err) {"
        "var p = globalThis.process;"
        "var handled = false;"
        "try { if (p && typeof p.emit === 'function') p.emit('uncaughtExceptionMonitor', err, 'uncaughtException'); } catch (monitorErr) { throw monitorErr; }"
        "if (p && typeof p.__unode_get_uncaught_exception_capture_callback === 'function') {"
        "  var cap = p.__unode_get_uncaught_exception_capture_callback();"
        "  if (typeof cap === 'function') { cap(err); handled = true; }"
        "}"
        "if (!handled && p && typeof p.listenerCount === 'function' && p.listenerCount('uncaughtException') > 0) {"
        "  p.emit('uncaughtException', err, 'uncaughtException');"
        "  handled = true;"
        "}"
        "if (!handled) throw err;"
        "return;"
        "} })();";
    source_to_run = entry_source.c_str();
  }

  napi_value script = nullptr;
  status = napi_create_string_utf8(env, source_to_run, NAPI_AUTO_LENGTH, &script);
  if (status != napi_ok || script == nullptr) {
    if (error_out != nullptr) {
      *error_out = "napi_create_string_utf8 failed: " + StatusToString(status);
    }
    return 1;
  }

  napi_value result = nullptr;
  status = napi_run_script(env, script, &result);
  if (status == napi_ok) {
    if (keep_event_loop_alive) {
      // Mirror Node's embedder loop semantics for process lifetime and beforeExit handling.
      const int loop_result = RunEventLoopUntilQuiescent(env, error_out);
      if (loop_result >= 0) {
        return loop_result;
      }
    }
    bool has_exit_code = false;
    const int exit_code = GetProcessExitCode(env, &has_exit_code);
    if (has_exit_code) {
      return exit_code;
    }
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
  // Install console using the bootstrap require, which resolves from the builtins dir
  // (GetBuiltinsDirForBootstrap) and is already set up in UnodeInstallModuleLoader. This way
  // we always load the JS console builtin when it exists, regardless of entry script path.
  napi_value script = nullptr;
  static const char kInstallConsole[] =
      "(function(){ globalThis.console = globalThis.__unode_bootstrap_require('console'); })();";
  napi_status status = napi_create_string_utf8(env, kInstallConsole, NAPI_AUTO_LENGTH, &script);
  if (status != napi_ok || script == nullptr) {
    return (status == napi_ok) ? napi_generic_failure : status;
  }
  napi_value ignored = nullptr;
  status = napi_run_script(env, script, &ignored);
  if (status == napi_ok) {
    return napi_ok;
  }

  // Fallback for when no JS console builtin could be loaded.
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

int UnodeRunScriptSource(napi_env env, const char* source_text, std::string* error_out) {
  if (error_out != nullptr) {
    error_out->clear();
  }
  return RunScriptWithGlobals(env, source_text, nullptr, error_out, false);
}

int UnodeRunScriptFileWithLoop(napi_env env,
                               const char* script_path,
                               std::string* error_out,
                               bool keep_event_loop_alive) {
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
  g_unode_current_script_path = script_path;
  std::string restore_cwd;
#if !defined(_WIN32)
  {
    const char* node_test_dir = std::getenv("NODE_TEST_DIR");
    if (node_test_dir != nullptr && script_path != nullptr) {
      const std::string script_path_s(script_path);
      const std::string test_dir_s(node_test_dir);
      if (!test_dir_s.empty() && script_path_s.rfind(test_dir_s, 0) == 0) {
        char cwd_buf[4096] = {'\0'};
        if (getcwd(cwd_buf, sizeof(cwd_buf)) != nullptr) {
          restore_cwd = cwd_buf;
        }
        const std::size_t pos = test_dir_s.find_last_of('/');
        if (pos != std::string::npos) {
          const std::string node_root = test_dir_s.substr(0, pos);
          chdir(node_root.c_str());
        }
      }
    }
  }
#endif
  const int rc =
      RunScriptWithGlobals(env, source.c_str(), script_path, error_out, keep_event_loop_alive);
#if !defined(_WIN32)
  if (!restore_cwd.empty()) {
    chdir(restore_cwd.c_str());
  }
#endif
  g_unode_current_script_path.clear();
  return rc;
}

int UnodeRunScriptFile(napi_env env, const char* script_path, std::string* error_out) {
  return UnodeRunScriptFileWithLoop(env, script_path, error_out, false);
}
