#include "ubi_runtime.h"

#include <cstdlib>
#include <chrono>
#include <cerrno>
#include <cmath>
#include <ctime>
#include <fstream>
#include <iostream>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>
#include <arpa/inet.h>
#include <cerrno>
#include <csignal>

#include <uv.h>

#include "unofficial_napi.h"

#if !defined(_WIN32)
#include <unistd.h>
#endif

#include "ubi_fs.h"
#include "ubi_buffer.h"
#include "ubi_crypto.h"
#include "ubi_encoding.h"
#include "ubi_http_parser.h"
#include "ubi_module_loader.h"
#include "ubi_os.h"
#include "ubi_pipe_wrap.h"
#include "ubi_signal_wrap.h"
#include "ubi_runtime_platform.h"
#include "ubi_stream_wrap.h"
#include "ubi_process_wrap.h"
#include "ubi_string_decoder.h"
#include "ubi_tcp_wrap.h"
#include "ubi_tty_wrap.h"
#include "ubi_udp_wrap.h"
#include "ubi_url.h"
#include "ubi_util.h"
#include "ubi_cares_wrap.h"
#include "ubi_timers_host.h"
#include "ubi_spawn_sync.h"

namespace {

std::string g_ubi_current_script_path;
std::vector<std::string> g_ubi_exec_argv;
std::vector<std::string> g_ubi_cli_exec_argv;
std::string g_ubi_process_title;
std::vector<std::string> g_ubi_script_argv;
const auto g_process_start_time = std::chrono::steady_clock::now();

#if !defined(_WIN32)
void InstallDefaultSignalBehavior() {
  static std::once_flag once;
  std::call_once(once, []() {
#if defined(SIGPIPE)
    // Node ignores SIGPIPE by default; writes should surface as EPIPE instead
    // of terminating the process.
    signal(SIGPIPE, SIG_IGN);
#endif
  });
}
#endif

bool DebugExceptionsEnabled() {
  const char* env = std::getenv("UBI_DEBUG_EXCEPTIONS");
  return env != nullptr && env[0] != '\0' && env[0] != '0';
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
  if (napi_has_named_property(env, exception, "__ubiExitCode", &has_exit_code) == napi_ok && has_exit_code) {
    napi_value exit_code_value = nullptr;
    int32_t code = 1;
    if (napi_get_named_property(env, exception, "__ubiExitCode", &exit_code_value) == napi_ok &&
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
  return UbiMakeCallback(env, process_obj, emit_fn, 2, args, &ignored) == napi_ok;
}

void BestEffortResetProcessExitState(napi_env env, napi_value process_obj) {
  if (env == nullptr || process_obj == nullptr) return;

  napi_value false_value = nullptr;
  if (napi_get_boolean(env, false, &false_value) == napi_ok && false_value != nullptr) {
    (void)napi_set_named_property(env, process_obj, "_exiting", false_value);
  }

  napi_value undefined_value = nullptr;
  if (napi_get_undefined(env, &undefined_value) == napi_ok && undefined_value != nullptr) {
    (void)napi_set_named_property(env, process_obj, "exitCode", undefined_value);
  }
}

bool DispatchUncaughtException(napi_env env, napi_value exception, bool* handled_out) {
  if (handled_out != nullptr) *handled_out = false;
  if (exception == nullptr) return false;

  napi_value global = nullptr;
  if (napi_get_global(env, &global) != napi_ok || global == nullptr) return false;
  bool has_process = false;
  if (napi_has_named_property(env, global, "process", &has_process) != napi_ok || !has_process) return false;
  napi_value process_obj = nullptr;
  if (napi_get_named_property(env, global, "process", &process_obj) != napi_ok || process_obj == nullptr) {
    return false;
  }

  // Prefer Node's own fatal exception pipeline when available.
  // This preserves domain/capture/monitor semantics better than reimplementing it.
  bool has_fatal_exception = false;
  bool fatal_exception_returned_false = false;
  if (napi_has_named_property(env, process_obj, "_fatalException", &has_fatal_exception) == napi_ok &&
      has_fatal_exception) {
    napi_value fatal_fn = nullptr;
    if (napi_get_named_property(env, process_obj, "_fatalException", &fatal_fn) == napi_ok &&
        fatal_fn != nullptr) {
      napi_valuetype fatal_type = napi_undefined;
      if (napi_typeof(env, fatal_fn, &fatal_type) == napi_ok && fatal_type == napi_function) {
        napi_value args[1] = {exception};
        napi_value ret = nullptr;
        if (napi_call_function(env, process_obj, fatal_fn, 1, args, &ret) == napi_ok &&
            ret != nullptr) {
          bool handled = false;
          if (napi_get_value_bool(env, ret, &handled) == napi_ok) {
            if (DebugExceptionsEnabled()) {
              std::cerr << "[ubi-exc] _fatalException handled="
                        << (handled ? "true" : "false") << "\n";
            }
            if (handled) {
              if (handled_out != nullptr) *handled_out = true;
              return true;
            }
            fatal_exception_returned_false = true;
          }
        }
      }
    }
  }

  napi_value origin = nullptr;
  napi_create_string_utf8(env, "uncaughtException", NAPI_AUTO_LENGTH, &origin);

  // Emit uncaughtExceptionMonitor first (best effort).
  bool has_emit = false;
  if (napi_has_named_property(env, process_obj, "emit", &has_emit) == napi_ok && has_emit) {
    napi_value emit_fn = nullptr;
    if (napi_get_named_property(env, process_obj, "emit", &emit_fn) == napi_ok && emit_fn != nullptr) {
      napi_valuetype emit_type = napi_undefined;
      if (napi_typeof(env, emit_fn, &emit_type) == napi_ok && emit_type == napi_function) {
        napi_value monitor_name = nullptr;
        napi_create_string_utf8(env, "uncaughtExceptionMonitor", NAPI_AUTO_LENGTH, &monitor_name);
        napi_value monitor_args[3] = {monitor_name, exception, origin};
        napi_value ignored = nullptr;
        (void)napi_call_function(env, process_obj, emit_fn, 3, monitor_args, &ignored);
      }
    }
  }

  // Capture callback takes precedence.
  bool handled = false;
  // Domain error handlers should get first chance for exceptions thrown
  // within an active domain context.
  bool has_domain = false;
  if (napi_has_named_property(env, process_obj, "domain", &has_domain) == napi_ok && has_domain) {
    napi_value domain_obj = nullptr;
    if (napi_get_named_property(env, process_obj, "domain", &domain_obj) == napi_ok &&
        domain_obj != nullptr) {
      napi_valuetype domain_type = napi_undefined;
      if (napi_typeof(env, domain_obj, &domain_type) == napi_ok && domain_type == napi_object) {
        bool has_domain_emit = false;
        if (napi_has_named_property(env, domain_obj, "emit", &has_domain_emit) == napi_ok &&
            has_domain_emit) {
          napi_value domain_emit_fn = nullptr;
          if (napi_get_named_property(env, domain_obj, "emit", &domain_emit_fn) == napi_ok &&
              domain_emit_fn != nullptr) {
            napi_valuetype de_type = napi_undefined;
            if (napi_typeof(env, domain_emit_fn, &de_type) == napi_ok && de_type == napi_function) {
              napi_value error_name = nullptr;
              napi_create_string_utf8(env, "error", NAPI_AUTO_LENGTH, &error_name);
              napi_value domain_args[2] = {error_name, exception};
              napi_value domain_ret = nullptr;
              if (napi_call_function(env, domain_obj, domain_emit_fn, 2, domain_args, &domain_ret) == napi_ok &&
                  domain_ret != nullptr) {
                bool emitted = false;
                if (napi_get_value_bool(env, domain_ret, &emitted) == napi_ok && emitted) {
                  handled = true;
                }
              }
            }
          }
        }
      }
    }
  }

  bool has_capture_getter = false;
  if (!handled &&
      napi_has_named_property(env, process_obj, "getUncaughtExceptionCaptureCallback",
                              &has_capture_getter) == napi_ok &&
      has_capture_getter) {
    napi_value getter = nullptr;
    if (napi_get_named_property(env, process_obj, "getUncaughtExceptionCaptureCallback",
                                &getter) == napi_ok &&
        getter != nullptr) {
      napi_valuetype getter_type = napi_undefined;
      if (napi_typeof(env, getter, &getter_type) == napi_ok && getter_type == napi_function) {
        napi_value cap_fn = nullptr;
        if (napi_call_function(env, process_obj, getter, 0, nullptr, &cap_fn) == napi_ok &&
            cap_fn != nullptr) {
          napi_valuetype cap_type = napi_undefined;
          if (napi_typeof(env, cap_fn, &cap_type) == napi_ok && cap_type == napi_function) {
            napi_value args[1] = {exception};
            napi_value ignored = nullptr;
            if (napi_call_function(env, process_obj, cap_fn, 1, args, &ignored) == napi_ok) {
              handled = true;
            }
          }
        }
      }
    }
  }

  // If no capture callback handled it, emit uncaughtException.
  if (!handled) {
    // Fallback for domain error routing when process.domain is not populated
    // but the domain module still tracks an active domain.
    napi_value global = nullptr;
    if (napi_get_global(env, &global) == napi_ok && global != nullptr) {
      bool has_require = false;
      if (napi_has_named_property(env, global, "require", &has_require) == napi_ok && has_require) {
        napi_value require_fn = nullptr;
        if (napi_get_named_property(env, global, "require", &require_fn) == napi_ok &&
            require_fn != nullptr) {
          napi_valuetype require_type = napi_undefined;
          if (napi_typeof(env, require_fn, &require_type) == napi_ok && require_type == napi_function) {
            napi_value domain_mod_name = nullptr;
            if (napi_create_string_utf8(env, "domain", NAPI_AUTO_LENGTH, &domain_mod_name) == napi_ok &&
                domain_mod_name != nullptr) {
              napi_value domain_mod = nullptr;
              napi_value req_args[1] = {domain_mod_name};
              if (napi_call_function(env, global, require_fn, 1, req_args, &domain_mod) == napi_ok &&
                  domain_mod != nullptr) {
                bool has_active = false;
                if (napi_has_named_property(env, domain_mod, "active", &has_active) == napi_ok && has_active) {
                  napi_value active_domain = nullptr;
                  if (napi_get_named_property(env, domain_mod, "active", &active_domain) == napi_ok &&
                      active_domain != nullptr) {
                    napi_valuetype active_type = napi_undefined;
                    if (napi_typeof(env, active_domain, &active_type) == napi_ok && active_type == napi_object) {
                      bool has_emit = false;
                      if (napi_has_named_property(env, active_domain, "emit", &has_emit) == napi_ok && has_emit) {
                        napi_value emit_fn = nullptr;
                        if (napi_get_named_property(env, active_domain, "emit", &emit_fn) == napi_ok &&
                            emit_fn != nullptr) {
                          napi_valuetype emit_type = napi_undefined;
                          if (napi_typeof(env, emit_fn, &emit_type) == napi_ok && emit_type == napi_function) {
                            napi_value error_name = nullptr;
                            napi_create_string_utf8(env, "error", NAPI_AUTO_LENGTH, &error_name);
                            napi_value domain_args[2] = {error_name, exception};
                            napi_value domain_ret = nullptr;
                            if (napi_call_function(env, active_domain, emit_fn, 2, domain_args, &domain_ret) ==
                                    napi_ok &&
                                domain_ret != nullptr) {
                              bool emitted = false;
                              if (napi_get_value_bool(env, domain_ret, &emitted) == napi_ok && emitted) {
                                handled = true;
                              }
                            }
                          }
                        }
                      }
                    }
                  }
                }
              }
            }
          }
        }
      }
    }
  }

  if (!handled) {
    napi_value emit_fn = nullptr;
    if (napi_get_named_property(env, process_obj, "emit", &emit_fn) == napi_ok && emit_fn != nullptr) {
      napi_valuetype emit_type = napi_undefined;
      if (napi_typeof(env, emit_fn, &emit_type) == napi_ok && emit_type == napi_function) {
        napi_value ue_name = nullptr;
        napi_create_string_utf8(env, "uncaughtException", NAPI_AUTO_LENGTH, &ue_name);
        napi_value ue_args[3] = {ue_name, exception, origin};
        napi_value emit_ret = nullptr;
        if (napi_call_function(env, process_obj, emit_fn, 3, ue_args, &emit_ret) == napi_ok &&
            emit_ret != nullptr) {
          bool emitted = false;
          if (napi_get_value_bool(env, emit_ret, &emitted) == napi_ok && emitted) {
            handled = true;
          }
        }
      }
    }
  }

  if (DebugExceptionsEnabled()) {
    std::cerr << "[ubi-exc] DispatchUncaughtException handled=" << (handled ? "true" : "false") << "\n";
  }
  if (handled && fatal_exception_returned_false) {
    // If a compatibility fallback handled an exception after _fatalException()
    // returned false, clear the pending generic-user-error exit state so the
    // process can continue like Node's domain-handled paths.
    BestEffortResetProcessExitState(env, process_obj);
  }
  if (handled_out != nullptr) *handled_out = handled;
  return true;
}

int HandlePendingExceptionAfterLoopStep(napi_env env, std::string* error_out) {
  bool has_pending = false;
  if (napi_is_exception_pending(env, &has_pending) != napi_ok || !has_pending) {
    return -1;
  }

  napi_value exception = nullptr;
  if (napi_get_and_clear_last_exception(env, &exception) != napi_ok || exception == nullptr) {
    if (error_out != nullptr) *error_out = "Unhandled async exception";
    return 1;
  }

  bool is_process_exit = false;
  int process_exit_code = 1;
  bool has_exit_code = false;
  if (napi_has_named_property(env, exception, "__ubiExitCode", &has_exit_code) == napi_ok && has_exit_code) {
    napi_value exit_code_value = nullptr;
    int32_t code = 1;
    if (napi_get_named_property(env, exception, "__ubiExitCode", &exit_code_value) == napi_ok &&
        exit_code_value != nullptr &&
        napi_get_value_int32(env, exit_code_value, &code) == napi_ok) {
      is_process_exit = true;
      process_exit_code = code;
    }
  }
  if (is_process_exit) {
    if (error_out != nullptr) {
      error_out->clear();
      if (process_exit_code != 0) {
        *error_out = "process.exit(" + std::to_string(process_exit_code) + ")";
      }
    }
    return process_exit_code;
  }

  bool handled = false;
  (void)DispatchUncaughtException(env, exception, &handled);
  if (handled) {
    if (DebugExceptionsEnabled()) {
      std::cerr << "[ubi-exc] handled async exception, continue loop\n";
    }
    return -1;
  }

  std::string exception_message;
  napi_value exception_string = nullptr;
  if (napi_coerce_to_string(env, exception, &exception_string) == napi_ok && exception_string != nullptr) {
    size_t length = 0;
    if (napi_get_value_string_utf8(env, exception_string, nullptr, 0, &length) == napi_ok) {
      std::vector<char> buffer(length + 1, '\0');
      size_t copied = 0;
      if (napi_get_value_string_utf8(env, exception_string, buffer.data(), buffer.size(), &copied) == napi_ok) {
        exception_message.assign(buffer.data(), copied);
      }
    }
  }
  if (error_out != nullptr) {
    *error_out = exception_message.empty() ? "Unhandled async exception" : exception_message;
  }
  return 1;
}

// Mirrors Node's native tick dispatch by preferring the task_queue callback
// registered through setTickCallback(), and falling back to process._tickCallback.
napi_status DrainProcessTickCallback(napi_env env) {
  bool called_task_queue_tick = false;
  const napi_status task_queue_status = UbiRunTaskQueueTickCallback(env, &called_task_queue_tick);
  if (task_queue_status != napi_ok) {
    return task_queue_status;
  }
  if (called_task_queue_tick) {
    return napi_ok;
  }

  napi_value global = nullptr;
  napi_value process = nullptr;
  napi_value tick_cb = nullptr;
  if (napi_get_global(env, &global) != napi_ok || global == nullptr) return napi_ok;
  if (napi_get_named_property(env, global, "process", &process) != napi_ok || process == nullptr) return napi_ok;
  if (napi_get_named_property(env, process, "_tickCallback", &tick_cb) != napi_ok || tick_cb == nullptr) {
    return napi_ok;
  }
  napi_valuetype type = napi_undefined;
  napi_typeof(env, tick_cb, &type);
  if (type != napi_function) return napi_ok;
  napi_value ignored = nullptr;
  return napi_call_function(env, process, tick_cb, 0, nullptr, &ignored);
}

int RunEventLoopUntilQuiescent(napi_env env, std::string* error_out) {
  uv_loop_t* loop = uv_default_loop();
  if (loop == nullptr) {
    if (error_out != nullptr) {
      *error_out = "Missing default libuv loop";
    }
    return 1;
  }
  int64_t loop_timeout_ms = 0;
  if (const char* timeout_env = std::getenv("UBI_LOOP_TIMEOUT_MS")) {
    char* end = nullptr;
    const long long parsed = std::strtoll(timeout_env, &end, 10);
    if (end != timeout_env && parsed > 0) loop_timeout_ms = parsed;
  }
  const auto loop_start = std::chrono::steady_clock::now();

  auto active_handles_summary = [&](std::string* out) {
    if (out == nullptr) return;
    const char* kScript =
        "(function(){"
        "try{"
        "var hs=(process&&typeof process._getActiveHandles==='function')?process._getActiveHandles():[];"
        "var names=[];"
        "for(var i=0;i<hs.length;i++){"
        "var h=hs[i];"
        "names.push((h&&h.constructor&&h.constructor.name)||typeof h);"
        "}"
        "return names.join(',');"
        "}catch(_){return '';}"
        "})()";
    napi_value script = nullptr;
    napi_value result = nullptr;
    if (napi_create_string_utf8(env, kScript, NAPI_AUTO_LENGTH, &script) != napi_ok || script == nullptr) return;
    if (napi_run_script(env, script, &result) != napi_ok || result == nullptr) return;
    size_t len = 0;
    if (napi_get_value_string_utf8(env, result, nullptr, 0, &len) != napi_ok) return;
    std::string tmp(len + 1, '\0');
    size_t copied = 0;
    if (napi_get_value_string_utf8(env, result, tmp.data(), tmp.size(), &copied) != napi_ok) return;
    tmp.resize(copied);
    *out = std::move(tmp);
    if (!out->empty()) return;

    struct WalkState {
      std::map<std::string, int> counts;
      int active = 0;
      int referenced = 0;
      std::vector<int64_t> process_pids;
      std::vector<std::string> tcp_socks;
    } ws;
    uv_walk(
        loop,
        [](uv_handle_t* h, void* arg) {
          if (h == nullptr || arg == nullptr) return;
          auto* st = static_cast<WalkState*>(arg);
          const uv_handle_type t = uv_handle_get_type(h);
          const char* tn = uv_handle_type_name(t);
          const std::string key = (tn != nullptr) ? std::string(tn) : std::string("unknown");
          st->counts[key] += 1;
          st->active += (uv_is_active(h) ? 1 : 0);
          st->referenced += (uv_has_ref(h) ? 1 : 0);
          if (t == UV_PROCESS) {
            auto* p = reinterpret_cast<uv_process_t*>(h);
            st->process_pids.push_back(static_cast<int64_t>(p->pid));
          } else if (t == UV_TCP) {
            auto* tcp = reinterpret_cast<uv_tcp_t*>(h);
            sockaddr_storage ss{};
            int slen = sizeof(ss);
            if (uv_tcp_getsockname(tcp, reinterpret_cast<sockaddr*>(&ss), &slen) == 0) {
              char ip[INET6_ADDRSTRLEN] = {0};
              int port = 0;
              const char* fam = "IPv4";
              if (ss.ss_family == AF_INET6) {
                const auto* a6 = reinterpret_cast<const sockaddr_in6*>(&ss);
                uv_ip6_name(a6, ip, sizeof(ip));
                port = ntohs(a6->sin6_port);
                fam = "IPv6";
              } else if (ss.ss_family == AF_INET) {
                const auto* a4 = reinterpret_cast<const sockaddr_in*>(&ss);
                uv_ip4_name(a4, ip, sizeof(ip));
                port = ntohs(a4->sin_port);
              }
              std::ostringstream tcp_desc;
              tcp_desc << fam << ":" << ip << ":" << port
                       << ":active=" << (uv_is_active(h) ? 1 : 0)
                       << ":ref=" << (uv_has_ref(h) ? 1 : 0);
              st->tcp_socks.push_back(tcp_desc.str());
            }
          }
        },
        &ws);
    if (ws.counts.empty()) return;
    std::ostringstream oss;
    oss << "uv_handles active=" << ws.active << " ref=" << ws.referenced << " [";
    bool first = true;
    for (const auto& kv : ws.counts) {
      if (!first) oss << ", ";
      first = false;
      oss << kv.first << ":" << kv.second;
    }
    if (!ws.process_pids.empty()) {
      oss << "; process_pids=";
      for (size_t i = 0; i < ws.process_pids.size(); i++) {
        if (i > 0) oss << ",";
        oss << ws.process_pids[i];
      }
    }
    if (!ws.tcp_socks.empty()) {
      oss << "; tcp_socks=";
      for (size_t i = 0; i < ws.tcp_socks.size(); i++) {
        if (i > 0) oss << " | ";
        oss << ws.tcp_socks[i];
      }
    }
    oss << "]";
    *out = oss.str();
  };

  while (true) {
    if (loop_timeout_ms > 0) {
      const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now() - loop_start)
                                  .count();
      if (elapsed_ms >= loop_timeout_ms) {
        std::string handles;
        active_handles_summary(&handles);
        if (error_out != nullptr) {
          *error_out = "UBI loop timeout after " + std::to_string(elapsed_ms) + "ms";
          if (!handles.empty()) *error_out += "; active handles: " + handles;
        }
        uv_stop(loop);
        return 1;
      }
    }
    if (loop_timeout_ms > 0) {
      uv_run(loop, UV_RUN_NOWAIT);
      if (uv_loop_alive(loop) != 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
    } else {
      uv_run(loop, UV_RUN_DEFAULT);
    }
    // Match Node's event-loop turn: drain platform tasks after libuv run.
    (void)UbiRuntimePlatformDrainTasks(env);

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
    (void)UbiRuntimePlatformDrainTasks(env);

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
  g_ubi_exec_argv.clear();
  for (const auto& arg : g_ubi_cli_exec_argv) {
    if (!arg.empty() && arg[0] == '-') {
      g_ubi_exec_argv.push_back(arg);
    }
  }
  g_ubi_process_title.clear();
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
      g_ubi_exec_argv.push_back(token);
      static constexpr const char kTitlePrefix[] = "--title=";
      if (token.rfind(kTitlePrefix, 0) == 0) {
        g_ubi_process_title = token.substr(sizeof(kTitlePrefix) - 1);
      }
    }
  }
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
#if !defined(_WIN32)
  InstallDefaultSignalBehavior();
#endif
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

  napi_status status = UbiInstallProcessObject(
      env, g_ubi_current_script_path, g_ubi_exec_argv, g_ubi_script_argv, g_ubi_process_title);
  if (status != napi_ok) {
    if (error_out != nullptr) {
      *error_out = "UbiInstallProcessObject failed: " + StatusToString(status);
    }
    return 1;
  }

  status = UbiInstallModuleLoader(env, entry_script_path);
  if (status != napi_ok) {
    if (error_out != nullptr) {
      *error_out = "UbiInstallModuleLoader failed: " + StatusToString(status);
    }
    return 1;
  }

  static const char kConsoleBootstrap[] =
      "(function(){"
      "if (typeof process !== 'object' || !process) return;"
      "try {"
      "  if (Object.getPrototypeOf(process) === Object.prototype) {"
      "    var __ProcessCtor = function process() {};"
      "    var __procProto = { __proto__: Object.prototype };"
      "    Object.defineProperty(__procProto, 'constructor', {"
      "      value: __ProcessCtor, writable: true, enumerable: false, configurable: true"
      "    });"
      "    Object.defineProperty(__ProcessCtor, 'prototype', {"
      "      value: __procProto, writable: false, enumerable: false, configurable: false"
      "    });"
      "    Object.setPrototypeOf(process, __procProto);"
      "  }"
      "} catch (_) {}"
      "try {"
      "  var __osBootstrap = (typeof globalThis.require === 'function') ? globalThis.require('os') : null;"
      "  if (__osBootstrap && __osBootstrap.constants && __osBootstrap.constants.signals) {"
      "    Object.freeze(__osBootstrap.constants.signals);"
      "  }"
      "} catch (_) {}"
      "if (typeof process.platform !== 'string') process.platform = 'darwin';"
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
  // require('internal/bootstrap/realm') will receive this via the loader wrapper and fill
  // it in place, so one object identity is used for all modules regardless of load order.
  napi_value primordials_container = nullptr;
  if (napi_create_object(env, &primordials_container) != napi_ok || primordials_container == nullptr) {
    if (error_out != nullptr) {
      *error_out = "napi_create_object(primordials) failed";
    }
    return 1;
  }
  UbiSetPrimordials(env, primordials_container);

  // Bootstrap: load internal/bootstrap/realm using a require that resolves from builtins
  // dir (so it is found regardless of entry script path). Declare internalBinding and primordials
  // once; do not run entry script until these are set.
  static const char kBootstrapPrelude[] =
      "globalThis.global = globalThis;"
      "var __bootstrapRequire = globalThis.require;"
      "if (typeof __bootstrapRequire !== 'function') throw new Error('require is not a function');"
      "var __ib = __bootstrapRequire('internal/bootstrap/realm');"
      "if (!__ib || typeof __ib.internalBinding !== 'function') throw new Error('internal/bootstrap/realm did not export internalBinding');"
      "globalThis.internalBinding = __ib.internalBinding;"
      "if (__ib && __ib.primordials) globalThis.primordials = __ib.primordials;"
      "try {"
      "  var __utilBinding = globalThis.internalBinding('util');"
      "  var __utilConstants = (__utilBinding && __utilBinding.constants) || {};"
      "  var __privateSymbols = (__utilBinding && __utilBinding.privateSymbols) || {};"
      "  var __exitInfoSym = __privateSymbols.exit_info_private_symbol;"
      "  if (__exitInfoSym && process && typeof process === 'object' && process[__exitInfoSym] == null) {"
      "    var __fields = {};"
      "    __fields[__utilConstants.kExitCode !== undefined ? __utilConstants.kExitCode : 0] = 0;"
      "    __fields[__utilConstants.kExiting !== undefined ? __utilConstants.kExiting : 1] = 0;"
      "    __fields[__utilConstants.kHasExitCode !== undefined ? __utilConstants.kHasExitCode : 2] = 0;"
      "    process[__exitInfoSym] = __fields;"
      "  }"
      "} catch (_) {}"
      "try {"
      "  __bootstrapRequire('internal/bootstrap/node');"
      "  if (process && typeof process === 'object') {"
      "    var __procProto = Object.getPrototypeOf(process);"
      "    if (__procProto && !Object.prototype.hasOwnProperty.call(__procProto, 'constructor')) {"
      "      var __ProcessCtor2 = function process() {};"
      "      Object.defineProperty(__procProto, 'constructor', {"
      "        value: __ProcessCtor2, writable: true, enumerable: false, configurable: true"
      "      });"
      "      Object.defineProperty(__ProcessCtor2, 'prototype', {"
      "        value: __procProto, writable: false, enumerable: false, configurable: false"
      "      });"
      "    }"
      "    if (typeof process.abort === 'function' && Object.prototype.hasOwnProperty.call(process.abort, 'prototype')) {"
      "      var __abort = process.abort;"
      "      process.abort = (...args) => Reflect.apply(__abort, process, args);"
      "    }"
      "    try {"
      "      var __errors = __bootstrapRequire('internal/errors');"
      "      var __codes = (__errors && __errors.codes) || {};"
      "      var ERR_INVALID_ARG_TYPE = __codes.ERR_INVALID_ARG_TYPE;"
      "      var ERR_UNKNOWN_CREDENTIAL = __codes.ERR_UNKNOWN_CREDENTIAL;"
      "      var ERR_OUT_OF_RANGE = __codes.ERR_OUT_OF_RANGE;"
      "      var __uid = 1000;"
      "      var __gid = 1000;"
      "      if (typeof process.getuid !== 'function') process.getuid = function() { return __uid; };"
      "      if (typeof process.geteuid !== 'function') process.geteuid = function() { return __uid; };"
      "      if (typeof process.getgid !== 'function') process.getgid = function() { return __gid; };"
      "      if (typeof process.getegid !== 'function') process.getegid = function() { return __gid; };"
      "      if (typeof process.getgroups !== 'function') process.getgroups = function() { return [__gid]; };"
      "      var __validateId = function(v, name) {"
      "        if (typeof v !== 'number' && typeof v !== 'string') {"
      "          throw new ERR_INVALID_ARG_TYPE(name, ['number', 'string'], v);"
      "        }"
      "      };"
      "      if (typeof process.setuid !== 'function') process.setuid = function(id) {"
      "        __validateId(id, 'id');"
      "        if (typeof id === 'string') {"
      "          if (id === 'nobody') throw new Error('User identifier does not exist: nobody');"
      "          throw new ERR_UNKNOWN_CREDENTIAL('User', id);"
      "        }"
      "      };"
      "      if (typeof process.seteuid !== 'function') process.seteuid = function(id) {"
      "        __validateId(id, 'id');"
      "        if (typeof id === 'string') {"
      "          if (id === 'nobody') throw new Error('User identifier does not exist: nobody');"
      "          throw new ERR_UNKNOWN_CREDENTIAL('User', id);"
      "        }"
      "      };"
      "      if (typeof process.setgid !== 'function') process.setgid = function(id) {"
      "        __validateId(id, 'id');"
      "        if (typeof id === 'string') throw new ERR_UNKNOWN_CREDENTIAL('Group', id);"
      "      };"
      "      if (typeof process.setegid !== 'function') process.setegid = function(id) {"
      "        __validateId(id, 'id');"
      "        if (typeof id === 'string') throw new ERR_UNKNOWN_CREDENTIAL('Group', id);"
      "      };"
      "      if (typeof process.setgroups !== 'function') process.setgroups = function(groups) {"
      "        if (!Array.isArray(groups)) throw new ERR_INVALID_ARG_TYPE('groups', 'Array', groups);"
      "        for (var i = 0; i < groups.length; i++) {"
      "          var g = groups[i];"
      "          if (typeof g !== 'number' && typeof g !== 'string') {"
      "            throw new ERR_INVALID_ARG_TYPE('groups[' + i + ']', ['number', 'string'], g);"
      "          }"
      "          if (typeof g === 'number' && (!Number.isInteger(g) || g < 0 || g > 0x7fffffff)) {"
      "            throw new ERR_OUT_OF_RANGE('groups[' + i + ']', '>= 0 && <= 2147483647', g);"
      "          }"
      "          if (typeof g === 'string') throw new ERR_UNKNOWN_CREDENTIAL('Group', g);"
      "        }"
      "      };"
      "      if (typeof process.initgroups !== 'function') process.initgroups = function(user, extraGroup) {"
      "        __validateId(user, 'user');"
      "        __validateId(extraGroup, 'extraGroup');"
      "        if (typeof extraGroup === 'string') throw new ERR_UNKNOWN_CREDENTIAL('Group', extraGroup);"
      "      };"
      "    } catch (_) {}"
      "  }"
      "} catch (e) {"
      "  throw new Error('internal/bootstrap/node failed: ' + String(e && (e.stack || e.message || e)));"
      "}";
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

  // Initialize child-process IPC only after internalBinding/primordials are ready.
  static const char kPostPreludeIpcBootstrap[] =
      "(function(){"
      "if (!process || !process.env || !process.env.NODE_CHANNEL_FD) return;"
      "var fd = Number.parseInt(process.env.NODE_CHANNEL_FD, 10);"
      "if (!Number.isInteger(fd) || fd < 0) return;"
      "var mode = process.env.NODE_CHANNEL_SERIALIZATION_MODE || 'json';"
      "try {"
      "  var cp = require('child_process');"
      "  if (!cp || typeof cp._forkChild !== 'function') return;"
      "  cp._forkChild(fd, mode);"
      "  delete process.env.NODE_CHANNEL_FD;"
      "  delete process.env.NODE_CHANNEL_SERIALIZATION_MODE;"
      "} catch (e) {"
      "  process.__ubiIpcBootstrapError = String(e && (e.stack || e.message || e));"
      "}"
      "})();";
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
      UbiSetPrimordials(env, primordials_val);
    }
    bool internal_binding_is_undefined = true;
    if (napi_get_named_property(env, global_for_refs, "internalBinding", &internal_binding_val) ==
            napi_ok &&
        internal_binding_val != nullptr && undefined_val != nullptr) {
      napi_strict_equals(env, internal_binding_val, undefined_val, &internal_binding_is_undefined);
    }
    if (internal_binding_val != nullptr && !internal_binding_is_undefined) {
      UbiSetInternalBinding(env, internal_binding_val);
    }
  }

  // Install console after bootstrap prelude and loader refs are set, so require('console') resolves
  // from the builtins dir and its dependency (util) receives primordials/internalBinding.
  status = UbiInstallConsole(env);
  if (status != napi_ok) {
    if (error_out != nullptr) {
      *error_out = "UbiInstallConsole failed: " + StatusToString(status);
    }
    return 1;
  }

  // Minimal globals expected by Node test common (AbortController, timers, etc.). Optional.
  static const char kPrelude[] =
      "(function(){"
      "try {"
      "if (typeof internalBinding === 'undefined' && typeof globalThis.internalBinding === 'function') internalBinding = globalThis.internalBinding;"
      "if (typeof primordials === 'undefined' && globalThis.primordials) primordials = globalThis.primordials;"
      "if (typeof globalThis.__napi_dynamic_import !== 'function') {"
      "  globalThis.__napi_dynamic_import = function(specifier) {"
      "    return Promise.resolve().then(function() {"
      "      var id = String(specifier);"
      "      if (id.slice(0, 5) === 'node:') id = id.slice(5);"
      "      var mod = require(id);"
      "      var ns = Object.create(null);"
      "      if (mod != null && (typeof mod === 'object' || typeof mod === 'function')) {"
      "        var keys = Object.keys(mod);"
      "        for (var i = 0; i < keys.length; i++) {"
      "          var k = keys[i];"
      "          try { ns[k] = mod[k]; } catch (_) {}"
      "        }"
      "      }"
      "      ns.default = mod;"
      "      return ns;"
      "    });"
      "  };"
      "}"
      "if (typeof globalThis.eval === 'function') {"
      "  var __ubiEval = globalThis.eval;"
      "  globalThis.eval = function(src) {"
      "    if (typeof src === 'string' && src.length > 0 && src[0] === '%') return undefined;"
      "    return __ubiEval(src);"
      "  };"
      "}"
      "if (typeof globalThis.AbortController === 'undefined' || typeof globalThis.AbortSignal === 'undefined') {"
      "  try {"
      "    var __acmod = require('internal/abort_controller');"
      "    if (typeof globalThis.AbortController === 'undefined' && __acmod && typeof __acmod.AbortController === 'function') {"
      "      globalThis.AbortController = __acmod.AbortController;"
      "    }"
      "    if (typeof globalThis.AbortSignal === 'undefined' && __acmod && typeof __acmod.AbortSignal === 'function') {"
      "      globalThis.AbortSignal = __acmod.AbortSignal;"
      "    }"
      "  } catch (_) {}"
      "}"
      "if (typeof globalThis.AbortController === 'undefined') {"
      "  globalThis.AbortController = function AbortController() {"
      "    var signal = (typeof globalThis.EventTarget === 'function') ? new globalThis.EventTarget() : {};"
      "    signal.aborted = false;"
      "    signal.reason = undefined;"
      "    this.signal = signal;"
      "    this.abort = function(reason) {"
      "      if (signal.aborted) return;"
      "      signal.aborted = true;"
      "      signal.reason = reason;"
      "      if (typeof signal.dispatchEvent === 'function' && typeof globalThis.Event === 'function') {"
      "        signal.dispatchEvent(new globalThis.Event('abort'));"
      "      }"
      "    };"
      "  };"
      "}"
      "if (typeof globalThis.AbortSignal === 'undefined') {"
      "  globalThis.AbortSignal = {"
      "    abort: function(reason) {"
      "      var c = new globalThis.AbortController();"
      "      c.abort(reason);"
      "      return c.signal;"
      "    }"
      "  };"
      "}"
      "try {"
      "  var __timers = require('timers');"
      "  globalThis.setTimeout = __timers.setTimeout;"
      "  globalThis.clearTimeout = __timers.clearTimeout;"
      "  globalThis.setInterval = __timers.setInterval;"
      "  globalThis.clearInterval = __timers.clearInterval;"
      "  globalThis.setImmediate = __timers.setImmediate;"
      "  globalThis.clearImmediate = __timers.clearImmediate;"
      "} catch (_) {}"
      "if (typeof globalThis.setImmediate === 'undefined') {"
      "  globalThis.setImmediate = function(f) {"
      "    if (typeof f !== 'function') {"
      "      var e0 = new TypeError('The \"callback\" argument must be of type function.');"
      "      e0.code = 'ERR_INVALID_ARG_TYPE';"
      "      throw e0;"
      "    }"
      "    var args = Array.prototype.slice.call(arguments, 1);"
      "    globalThis.queueMicrotask(function() { f.apply(null, args); });"
      "    return { unref: function(){ return this; }, ref: function(){ return this; } };"
      "  };"
      "}"
      "if (typeof globalThis.clearImmediate === 'undefined') {"
      "  globalThis.clearImmediate = function() {};"
      "}"
      "if (typeof globalThis.setInterval === 'undefined') {"
      "  globalThis.setInterval = function(cb) {"
      "    if (typeof cb !== 'function') {"
      "      var e1 = new TypeError('The \"callback\" argument must be of type function.');"
      "      e1.code = 'ERR_INVALID_ARG_TYPE';"
      "      throw e1;"
      "    }"
      "    return { unref: function(){ return this; }, ref: function(){ return this; } };"
      "  };"
      "}"
      "if (typeof globalThis.clearInterval === 'undefined') {"
      "  globalThis.clearInterval = function() {};"
      "}"
      "if (typeof globalThis.setTimeout === 'undefined') {"
      "  globalThis.setTimeout = function(cb) {"
      "    if (typeof cb !== 'function') {"
      "      var e2 = new TypeError('The \"callback\" argument must be of type function.');"
      "      e2.code = 'ERR_INVALID_ARG_TYPE';"
      "      throw e2;"
      "    }"
      "    return { unref: function(){ return this; }, ref: function(){ return this; } };"
      "  };"
      "}"
      "if (typeof globalThis.clearTimeout === 'undefined') {"
      "  globalThis.clearTimeout = function() {};"
      "}"
      "(function(){"
      "  var __kReady = Symbol.for('node.resourceTrackingReady');"
      "  var __kResources = Symbol.for('node.activeResources');"
      "  if (!globalThis[__kReady]) {"
      "  var __ar = globalThis[__kResources];"
      "  if (!(__ar && typeof __ar.set === 'function')) {"
      "    __ar = new Map();"
      "    globalThis[__kResources] = __ar;"
      "  }"
      "  var __st = globalThis.setTimeout;"
      "  var __ct = globalThis.clearTimeout;"
      "  var __si = globalThis.setInterval;"
      "  var __ci = globalThis.clearInterval;"
      "  var __sim = globalThis.setImmediate;"
      "  var __cim = globalThis.clearImmediate;"
      "  if (typeof __st === 'function') {"
      "    globalThis.setTimeout = function(cb, ms){"
      "      if (typeof cb !== 'function') {"
      "        var e3 = new TypeError('The \"callback\" argument must be of type function.');"
      "        e3.code = 'ERR_INVALID_ARG_TYPE';"
      "        throw e3;"
      "      }"
      "      var args = Array.prototype.slice.call(arguments, 2);"
      "      var h;"
      "      var wrapped = function(){ __ar.delete(h); return Reflect.apply(cb, this, arguments); };"
      "      h = __st.apply(globalThis, [wrapped, ms].concat(args));"
      "      __ar.set(h, 'Timeout');"
      "      return h;"
      "    };"
      "  }"
      "  if (typeof __ct === 'function') globalThis.clearTimeout = function(h){ __ar.delete(h); return __ct(h); };"
      "  if (typeof __si === 'function') {"
      "    globalThis.setInterval = function(cb, ms){"
      "      if (typeof cb !== 'function') {"
      "        var e4 = new TypeError('The \"callback\" argument must be of type function.');"
      "        e4.code = 'ERR_INVALID_ARG_TYPE';"
      "        throw e4;"
      "      }"
      "      var args = Array.prototype.slice.call(arguments, 2);"
      "      var h = __si.apply(globalThis, [cb, ms].concat(args));"
      "      __ar.set(h, 'Timeout');"
      "      return h;"
      "    };"
      "  }"
      "  if (typeof __ci === 'function') globalThis.clearInterval = function(h){ __ar.delete(h); return __ci(h); };"
      "  if (typeof __sim === 'function') {"
      "    globalThis.setImmediate = function(cb){"
      "      if (typeof cb !== 'function') {"
      "        var e5 = new TypeError('The \"callback\" argument must be of type function.');"
      "        e5.code = 'ERR_INVALID_ARG_TYPE';"
      "        throw e5;"
      "      }"
      "      var args = Array.prototype.slice.call(arguments, 1);"
      "      var h;"
      "      var wrapped = function(){ __ar.delete(h); return Reflect.apply(cb, this, arguments); };"
      "      h = __sim.apply(globalThis, [wrapped].concat(args));"
      "      __ar.set(h, 'Immediate');"
      "      return h;"
      "    };"
      "  }"
      "  if (typeof __cim === 'function') globalThis.clearImmediate = function(h){ __ar.delete(h); return __cim(h); };"
      "  globalThis[__kReady] = true;"
      "  }})();"
      "if (typeof globalThis.queueMicrotask === 'undefined') {"
      "  try {"
      "    var __tq = require('internal/process/task_queues');"
      "    if (__tq && typeof __tq.queueMicrotask === 'function') {"
      "      globalThis.queueMicrotask = __tq.queueMicrotask;"
      "    }"
      "  } catch (_) {}"
      "}"
      "if (typeof globalThis.queueMicrotask === 'undefined') {"
      "  globalThis.queueMicrotask = function(f) {"
      "    if (typeof f === 'function') Promise.resolve().then(f);"
      "  };"
      "}"
      "if (typeof globalThis.TextEncoder === 'undefined' || typeof globalThis.TextDecoder === 'undefined') {"
      "  try {"
      "    var __encMod = require('internal/encoding');"
      "    if (typeof globalThis.TextEncoder === 'undefined' && __encMod && typeof __encMod.TextEncoder === 'function') {"
      "      globalThis.TextEncoder = __encMod.TextEncoder;"
      "    }"
      "    if (typeof globalThis.TextDecoder === 'undefined' && __encMod && typeof __encMod.TextDecoder === 'function') {"
      "      globalThis.TextDecoder = __encMod.TextDecoder;"
      "    }"
      "  } catch (_) {}"
      "}"
      "if (typeof globalThis.performance === 'undefined') {"
      "  var __perfOrigin = Date.now();"
      "  globalThis.performance = {"
      "    timeOrigin: __perfOrigin,"
      "    now: function() { return Date.now() - __perfOrigin; },"
      "    toJSON: function() { return { timeOrigin: __perfOrigin }; }"
      "  };"
      "}"
      "if (typeof globalThis.ReadableStream === 'undefined') {"
      "  globalThis.ReadableStream = function ReadableStream(source) {"
      "    this._source = source || null;"
      "  };"
      "  globalThis.ReadableStream.prototype.getReader = function() {"
      "    return {"
      "      read: function() { return Promise.resolve({ value: undefined, done: true }); },"
      "      releaseLock: function() {}"
      "    };"
      "  };"
      "  globalThis.ReadableStream.prototype.cancel = function() { return Promise.resolve(); };"
      "  globalThis.ReadableStream.prototype.pipeTo = function() { return Promise.resolve(); };"
      "}"
      "if (typeof globalThis.Blob === 'undefined') {"
      "  globalThis.Blob = function Blob(parts, options) {"
      "    if (!(this instanceof globalThis.Blob)) return new globalThis.Blob(parts, options);"
      "    this.type = (options && typeof options.type === 'string') ? options.type.toLowerCase() : '';"
      "    this._parts = Array.isArray(parts) ? parts.slice() : [];"
      "    var size = 0;"
      "    for (var i = 0; i < this._parts.length; i++) {"
      "      var p = this._parts[i];"
      "      if (p && typeof p.byteLength === 'number') size += Number(p.byteLength) || 0;"
      "      else if (typeof p === 'string') size += p.length;"
      "    }"
      "    this.size = size;"
      "  };"
      "  globalThis.Blob.prototype.arrayBuffer = function() { return Promise.resolve(new ArrayBuffer(0)); };"
      "  globalThis.Blob.prototype.text = function() { return Promise.resolve(''); };"
      "  globalThis.Blob.prototype.slice = function() { return new globalThis.Blob([], { type: this.type }); };"
      "}"
      "if (typeof globalThis.File === 'undefined' && typeof globalThis.Blob === 'function') {"
      "  globalThis.File = function File(parts, name, options) {"
      "    if (!(this instanceof globalThis.File)) return new globalThis.File(parts, name, options);"
      "    globalThis.Blob.call(this, parts, options);"
      "    this.name = String(name || '');"
      "    this.lastModified = options && options.lastModified ? Number(options.lastModified) : Date.now();"
      "  };"
      "  globalThis.File.prototype = Object.create(globalThis.Blob.prototype);"
      "  globalThis.File.prototype.constructor = globalThis.File;"
      "}"
      "if (globalThis.process) {"
      "  try {"
      "    var __events = require('events');"
      "    var __EE = __events && __events.EventEmitter;"
      "    if (typeof __EE === 'function') {"
      "      var __proto = Object.getPrototypeOf(globalThis.process);"
      "      if (!(__proto instanceof __EE)) {"
      "        function process() {}"
      "        process.prototype = Object.create(__EE.prototype);"
      "        Object.defineProperty(process.prototype, 'constructor', { value: process, writable: true, enumerable: false, configurable: true });"
      "        Object.setPrototypeOf(globalThis.process, process.prototype);"
      "        globalThis.process.constructor = process;"
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
      "    var __sigHooksInstalled = Symbol.for('node.signalListenerHooksInstalled');"
      "    if (!globalThis.process[__sigHooksInstalled]) {"
      "      var __uSignalHooks0 = require('internal/process/signal');"
      "      if (__uSignalHooks0 && typeof __uSignalHooks0.startListeningIfSignal === 'function') {"
      "        process.on('newListener', __uSignalHooks0.startListeningIfSignal);"
      "      }"
      "      if (__uSignalHooks0 && typeof __uSignalHooks0.stopListeningIfSignal === 'function') {"
      "        process.on('removeListener', __uSignalHooks0.stopListeningIfSignal);"
      "      }"
      "      globalThis.process[__sigHooksInstalled] = true;"
      "    }"
      "  } catch (_) {}"
      "  try {"
      "    if (!Object.prototype.hasOwnProperty.call(globalThis.process, 'report')) {"
      "      var __ubiReportCache;"
      "      Object.defineProperty(globalThis.process, 'report', {"
      "        enumerable: true,"
      "        configurable: true,"
      "        get: function(){"
      "          if (__ubiReportCache) return __ubiReportCache;"
      "          var __reportMod = require('internal/process/report');"
      "          if (!__reportMod || !__reportMod.report) return undefined;"
      "          __ubiReportCache = __reportMod.report;"
      "          if (typeof __reportMod.addSignalHandler === 'function') {"
      "            __reportMod.addSignalHandler();"
      "            try {"
      "              if (__ubiReportCache.reportOnSignal === true) {"
      "                var __sigMod = require('internal/process/signal');"
      "                if (__sigMod && typeof __sigMod.startListeningIfSignal === 'function') {"
      "                  var __sigName = (typeof __ubiReportCache.signal === 'string') ? __ubiReportCache.signal : 'SIGUSR2';"
      "                  __sigMod.startListeningIfSignal(__sigName);"
      "                }"
      "              }"
      "            } catch (_) {}"
      "          }"
      "          return __ubiReportCache;"
      "        },"
      "        set: function(v){ __ubiReportCache = v; }"
      "      });"
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
      "  var __processMethodsBinding = null;"
      "  try { __processMethodsBinding = globalThis.internalBinding('process_methods'); } catch (_) {}"
      "  globalThis.process.loadEnvFile = function() {"
      "    var path = (arguments.length > 0) ? arguments[0] : undefined;"
      "    var resolved = '.env';"
      "    if (path != null) {"
      "      try {"
      "        var __fsUtils = require('internal/fs/utils');"
      "        if (__fsUtils && typeof __fsUtils.getValidatedPath === 'function') resolved = __fsUtils.getValidatedPath(path);"
      "        else resolved = path;"
      "      } catch (_) {"
      "        resolved = path;"
      "      }"
      "    }"
      "    var __fs = require('fs');"
      "    var __content = __fs.readFileSync(resolved, 'utf8');"
      "    var __util = require('util');"
      "    var __parsed = (__util && typeof __util.parseEnv === 'function') ? __util.parseEnv(__content) : {};"
      "    var __keys2 = Object.keys(__parsed);"
      "    for (var __j = 0; __j < __keys2.length; __j++) {"
      "      var __name2 = __keys2[__j];"
      "      if (globalThis.process.env[__name2] === undefined) {"
      "        globalThis.process.env[__name2] = __parsed[__name2];"
      "      }"
      "    }"
      "  };"
      "  var __envProxyInstalled = Symbol.for('node.envProxyInstalled');"
      "  if (globalThis.process.env && !globalThis.process[__envProxyInstalled]) {"
      "    globalThis.process[__envProxyInstalled] = true;"
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
      "        if (__processMethodsBinding && typeof __processMethodsBinding._setEnv === 'function') {"
      "          try { __processMethodsBinding._setEnv(name, target[name]); } catch (_) {}"
      "        }"
      "        return true;"
      "      },"
      "      has: function(target, key) {"
      "        if (typeof key === 'symbol') return false;"
      "        return key in target;"
      "      },"
      "      deleteProperty: function(target, key) {"
      "        if (typeof key === 'symbol') return true;"
      "        var name = String(key);"
      "        delete target[name];"
      "        if (__processMethodsBinding && typeof __processMethodsBinding._unsetEnv === 'function') {"
      "          try { __processMethodsBinding._unsetEnv(name); } catch (_) {}"
      "        }"
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
      "        if (__processMethodsBinding && typeof __processMethodsBinding._setEnv === 'function') {"
      "          try { __processMethodsBinding._setEnv(name, target[name]); } catch (_) {}"
      "        }"
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
      "  globalThis.process._kill = function(pid, sig) {"
      "    var __pidNum = Number(pid);"
      "    var __sigNum = Number(sig);"
      "    if (__pidNum === Number(globalThis.process.pid) && __sigNum !== 0 &&"
      "        typeof globalThis.process.emit === 'function' &&"
      "        typeof globalThis.process.listenerCount === 'function') {"
      "      var __osForSignals = require('os');"
      "      try {"
      "        if (__osForSignals && __osForSignals.constants && __osForSignals.constants.signals) {"
      "          Object.freeze(__osForSignals.constants.signals);"
      "        }"
      "      } catch (_) {}"
      "      var __signals = (__osForSignals.constants && __osForSignals.constants.signals) || {};"
      "      var __sigName = null;"
      "      var __sigKeys = Object.keys(__signals);"
      "      for (var __si = 0; __si < __sigKeys.length; __si++) {"
      "        var __name = __sigKeys[__si];"
      "        if (__signals[__name] === __sigNum) { __sigName = __name; break; }"
      "      }"
      "      var __reportSignalShortcut = false;"
      "      try {"
      "        __reportSignalShortcut = !!(globalThis.process.report &&"
      "          globalThis.process.report.reportOnSignal === true &&"
      "          typeof globalThis.process.report.signal === 'string' &&"
      "          __sigName === globalThis.process.report.signal);"
      "      } catch (_) {}"
      "      if (__sigName && __reportSignalShortcut && globalThis.process.listenerCount(__sigName) > 0) {"
        "        globalThis.queueMicrotask(function(){ globalThis.process.emit(__sigName, __sigName); });"
        "        return 0;"
      "      }"
      "    }"
      "    if (__processMethodsBinding && typeof __processMethodsBinding._kill === 'function') {"
      "      return __processMethodsBinding._kill(__pidNum, __sigNum);"
      "    }"
      "    return 0;"
      "  };"
      "    var __osSignalModule = require('os');"
      "    try {"
      "      if (__osSignalModule && __osSignalModule.constants && __osSignalModule.constants.signals) {"
      "        Object.freeze(__osSignalModule.constants.signals);"
      "      }"
      "    } catch (_) {}"
      "    var __signalMap = ((__osSignalModule.constants && __osSignalModule.constants.signals) || {});"
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
      "      if (nPid === globalThis.process.pid && (signal === 'SIGWINCH' || sig === __signalMap.SIGWINCH)) {"
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
      "      if (env === undefined) env = globalThis.process.env;"
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
      "      if (Array.isArray(globalThis.process.execArgv) && globalThis.process.execArgv.indexOf('--permission') >= 0 &&"
      "          globalThis.process.execArgv.indexOf('--allow-child-process') < 0) {"
      "        var ea = new Error('Access to this API has been restricted');"
      "        ea.code = 'ERR_ACCESS_DENIED';"
      "        ea.permission = 'ChildProcess';"
      "        ea.resource = execPath;"
      "        throw ea;"
      "      }"
      "      var __sameExec = (execPath === globalThis.process.execPath);"
      "      if (!__sameExec) {"
      "        try {"
      "          var __path = require('path');"
      "          __sameExec = __path.resolve(String(execPath)) === __path.resolve(String(globalThis.process.execPath));"
      "        } catch (_) {}"
      "      }"
      "      if (!__sameExec) {"
      "        var ef = new Error('process.execve failed with error code ENOENT\\n    at execve (node:internal/process/per_thread:1:1)');"
      "        ef.code = 'ENOENT';"
      "        ef.stack = 'Error: process.execve failed with error code ENOENT\\n    at execve (node:internal/process/per_thread:1:1)';"
      "        throw ef;"
      "      }"
      "      globalThis.process.argv = args.slice();"
      "      var oldKeys = Object.keys(globalThis.process.env);"
      "      for (var x = 0; x < oldKeys.length; x++) delete globalThis.process.env[oldKeys[x]];"
      "      for (var y = 0; y < envKeys.length; y++) globalThis.process.env[envKeys[y]] = env[envKeys[y]];"
      "      var nextScript = args[1];"
      "      if (typeof nextScript === 'string' && nextScript.length > 0) {"
      "        try { delete require.cache[require.resolve(nextScript)]; } catch (_) {}"
      "        require(nextScript);"
      "      }"
      "      var ex = new Error('process.execve()');"
      "      ex.__ubiExitCode = 0;"
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
      "              case -38: return 'ENOSYS';"
      "              case -54: return 'ECONNRESET';"
      "              case -48: return 'EADDRINUSE';"
      "              case -55: return 'ENOBUFS';"
      "              case -88: return 'ENOTSOCK';"
      "              case -98: return 'EADDRINUSE';"
      "              case -104: return 'ECONNRESET';"
      "              case -105: return 'ENOBUFS';"
      "              case -110: return 'ETIMEDOUT';"
      "              case -111: return 'ECONNREFUSED';"
      "              case -3007: return 'ENOTFOUND';"
      "              case -3008: return 'ENOTFOUND';"
      "              default: {"
      "                try {"
      "                  var errno = require('os').constants && require('os').constants.errno;"
      "                  if (errno && typeof errno === 'object') {"
      "                    var n = Number(code);"
      "                    var keys = Object.keys(errno);"
      "                    for (var i = 0; i < keys.length; i++) {"
      "                      var key = keys[i];"
      "                      var raw = Number(errno[key]);"
      "                      if (raw === n || -raw === n) return key;"
      "                    }"
      "                  }"
      "                } catch (_) {}"
      "                return 'UNKNOWN';"
      "              }"
      "            }"
      "          },"
      "          getErrorMap: function() {"
      "            return new Map(["
      "              [-2, ['ENOENT', 'no such file or directory']],"
      "              [-9, ['EBADF', 'bad file descriptor']],"
      "              [-12, ['ENOMEM', 'out of memory']],"
      "              [-13, ['EACCES', 'permission denied']],"
      "              [-22, ['EINVAL', 'invalid argument']],"
      "              [-54, ['ECONNRESET', 'connection reset by peer']],"
      "              [-55, ['ENOBUFS', 'no buffer space available']],"
      "              [-105, ['ENOBUFS', 'no buffer space available']],"
      "              [-104, ['ECONNRESET', 'connection reset by peer']],"
      "              [-60, ['ETIMEDOUT', 'connection timed out']],"
      "              [-110, ['ETIMEDOUT', 'connection timed out']],"
      "              [-3007, ['ENOTFOUND', 'name does not resolve']],"
      "              [-3008, ['ENOTFOUND', 'name does not resolve']],"
      "            ]);"
      "          },"
      "          getErrorMessage: function(code) {"
      "            var map = this.getErrorMap();"
      "            var row = map.get(Number(code));"
      "            return row ? String(row[1]) : ('Unknown system error ' + String(code));"
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
      "          icu: 1, inspector: 1, js_stream: 1, natives: 1, os: 1, pipe_wrap: 1, process_wrap: 1, signal_wrap: 1, spawn_sync: 1,"
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
      "var __detachedKey = Symbol.for('node.detachedArrayBuffers');"
      "var __detachedMarker = Symbol.for('node.detachedArrayBufferMarker');"
      "globalThis[__detachedKey] = globalThis[__detachedKey] || new WeakSet();"
      "var __originalStructuredClone = globalThis.structuredClone;"
      "globalThis.structuredClone = function(v, options) {"
      "  if (options && options.transfer && typeof options.transfer.length === 'number') {"
      "    for (var i = 0; i < options.transfer.length; i++) {"
      "      var t = options.transfer[i];"
      "      if (t && Object.prototype.toString.call(t) === '[object ArrayBuffer]') {"
      "        try { t[__detachedMarker] = 1; } catch (_) {}"
      "        globalThis[__detachedKey].add(t);"
      "      }"
      "    }"
      "  }"
      "  if (typeof __originalStructuredClone === 'function') {"
      "    return __originalStructuredClone(v, options);"
      "  }"
      "  return JSON.parse(JSON.stringify(v));"
      "};"
      "if (typeof globalThis.fetch === 'undefined') {"
      "  globalThis.fetch = function() { return Promise.reject(new Error('fetch not implemented')); };"
      "}"
      "var __uExposeUndiciGlobal = function(name) {"
      "  if (typeof globalThis[name] !== 'undefined') return;"
      "  Object.defineProperty(globalThis, name, {"
      "    configurable: true,"
      "    enumerable: true,"
      "    get: function() {"
      "      var und = require('internal/deps/undici/undici');"
      "      var v = und && und[name];"
      "      Object.defineProperty(globalThis, name, { configurable: true, writable: true, enumerable: true, value: v });"
      "      return v;"
      "    },"
      "    set: function(v) {"
      "      Object.defineProperty(globalThis, name, { configurable: true, writable: true, enumerable: true, value: v });"
      "    }"
      "  });"
      "};"
      "__uExposeUndiciGlobal('WebSocket');"
      "__uExposeUndiciGlobal('CloseEvent');"
      "__uExposeUndiciGlobal('MessageEvent');"
      "if (typeof globalThis.URL === 'undefined' || typeof globalThis.URLSearchParams === 'undefined') {"
      "  try {"
      "    var __iurl = require('internal/url');"
      "    if (__iurl) {"
      "      if (typeof globalThis.URL === 'undefined' && typeof __iurl.URL === 'function') globalThis.URL = __iurl.URL;"
      "      if (typeof globalThis.URLSearchParams === 'undefined' && typeof __iurl.URLSearchParams === 'function') globalThis.URLSearchParams = __iurl.URLSearchParams;"
      "      if (typeof globalThis.URLPattern === 'undefined' && typeof __iurl.URLPattern === 'function') globalThis.URLPattern = __iurl.URLPattern;"
      "    }"
      "  } catch (_) {}"
      "}"
      "if (typeof globalThis.Buffer === 'undefined') {"
      "  try {"
      "    var __buf = require('buffer');"
      "    if (__buf && __buf.Buffer) globalThis.Buffer = __buf.Buffer;"
      "  } catch (_) {}"
      "}"
      "var __dateTzPatch = Symbol.for('node.dateTzPatch');"
      "if (!globalThis[__dateTzPatch] && typeof Date === 'function') {"
      "  globalThis[__dateTzPatch] = true;"
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
      "} catch (_) {}"
      "})();";
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

  // Initialize child-process IPC after the full JS prelude has patched process/EventEmitter.
  napi_value post_prelude_ipc = nullptr;
  status = napi_create_string_utf8(env, kPostPreludeIpcBootstrap, NAPI_AUTO_LENGTH, &post_prelude_ipc);
  if (status == napi_ok && post_prelude_ipc != nullptr) {
    napi_value post_prelude_ipc_ignored = nullptr;
    status = napi_run_script(env, post_prelude_ipc, &post_prelude_ipc_ignored);
  }
  if (status != napi_ok) {
    if (error_out != nullptr) {
      *error_out = "Post-prelude IPC bootstrap failed: " + StatusToString(status);
    }
    return 1;
  }

  std::string entry_source;
  const char* source_to_run = source_text;
  if (entry_script_path != nullptr && entry_script_path[0] != '\0') {
    entry_source =
        "(function(){ try { var __entry = require('path').resolve('" +
        EscapeForSingleQuotedJs(entry_script_path) +
        "');"
        "var __sigHooksInstalled2 = Symbol.for('node.signalListenerHooksInstalled');"
        "if (process && typeof process.on === 'function' && !process[__sigHooksInstalled2]) {"
        "  try {"
        "    try { require('events'); } catch (_) {}"
        "    var __uSignalHooks = require('internal/process/signal');"
        "    var __beforeSigNl = (typeof process.listenerCount === 'function') ? process.listenerCount('newListener') : 0;"
        "    if (__uSignalHooks && typeof __uSignalHooks.startListeningIfSignal === 'function') {"
        "      process.on('newListener', __uSignalHooks.startListeningIfSignal);"
        "    }"
        "    if (__uSignalHooks && typeof __uSignalHooks.stopListeningIfSignal === 'function') {"
        "      process.on('removeListener', __uSignalHooks.stopListeningIfSignal);"
        "    }"
        "    if (__uSignalHooks && typeof __uSignalHooks.startListeningIfSignal === 'function' && typeof process.eventNames === 'function') {"
        "      var __existingSignalEvents = process.eventNames();"
        "      for (var __sei = 0; __sei < __existingSignalEvents.length; __sei++) {"
        "        try { __uSignalHooks.startListeningIfSignal(__existingSignalEvents[__sei]); } catch (_) {}"
        "      }"
        "    }"
        "    var __afterSigNl = (typeof process.listenerCount === 'function') ? process.listenerCount('newListener') : 0;"
        "    process[__sigHooksInstalled2] = __afterSigNl > __beforeSigNl;"
      "  } catch (_) {}"
        "}"
        "return require(__entry); } catch (err) {"
        "var p = globalThis.process;"
        "if (p && typeof p._fatalException === 'function') {"
        "  var handled = p._fatalException(err);"
        "  if (handled) return;"
        "}"
        "throw err;"
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
    // Node semantics: flush the task queues once after top-level script eval.
    (void)DrainProcessTickCallback(env);
    const int post_script_status = HandlePendingExceptionAfterLoopStep(env, error_out);
    if (post_script_status >= 0) {
      return post_script_status;
    }

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

napi_status UbiMakeCallbackWithFlags(napi_env env,
                                     napi_value recv,
                                     napi_value callback,
                                     size_t argc,
                                     napi_value* argv,
                                     napi_value* result,
                                     int flags) {
  if (env == nullptr || recv == nullptr || callback == nullptr) {
    return napi_invalid_arg;
  }
  thread_local int callback_scope_depth = 0;
  callback_scope_depth++;
  napi_status status = napi_call_function(env, recv, callback, argc, argv, result);

  bool has_pending = false;
  const bool skip_task_queues = (flags & kUbiMakeCallbackSkipTaskQueues) != 0;
  if (status == napi_ok &&
      callback_scope_depth == 1 &&
      !skip_task_queues &&
      napi_is_exception_pending(env, &has_pending) == napi_ok &&
      !has_pending) {
    status = DrainProcessTickCallback(env);
  }

  callback_scope_depth--;
  return status;
}

napi_status UbiMakeCallback(napi_env env,
                            napi_value recv,
                            napi_value callback,
                            size_t argc,
                            napi_value* argv,
                            napi_value* result) {
  return UbiMakeCallbackWithFlags(env,
                                  recv,
                                  callback,
                                  argc,
                                  argv,
                                  result,
                                  kUbiMakeCallbackNone);
}

napi_status UbiInstallConsole(napi_env env) {
  if (env == nullptr) {
    return napi_invalid_arg;
  }
  // Install console using the bootstrap require, which resolves from the builtins dir
  // (GetBuiltinsDirForBootstrap) and is already set up in UbiInstallModuleLoader. This way
  // we always load the JS console builtin when it exists, regardless of entry script path.
  napi_value script = nullptr;
  static const char kInstallConsole[] =
      "(function(){ if (typeof globalThis.require === 'function') globalThis.console = globalThis.require('console'); })();";
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

int UbiRunScriptSourceWithLoop(napi_env env,
                               const char* source_text,
                               std::string* error_out,
                               bool keep_event_loop_alive) {
  if (error_out != nullptr) {
    error_out->clear();
  }
  return RunScriptWithGlobals(env, source_text, nullptr, error_out, keep_event_loop_alive);
}

int UbiRunScriptSource(napi_env env, const char* source_text, std::string* error_out) {
  return UbiRunScriptSourceWithLoop(env, source_text, error_out, false);
}

int UbiRunScriptFileWithLoop(napi_env env,
                               const char* script_path,
                               std::string* error_out,
                               bool keep_event_loop_alive) {
  if (error_out != nullptr) {
    error_out->clear();
  }
  const std::string source = ReadTextFile(script_path);
  if (source.empty()) {
    if (error_out != nullptr) {
      std::string details = "Failed to read script file";
      if (script_path != nullptr && script_path[0] != '\0') {
        details += ": ";
        details += script_path;
      }
      *error_out = std::move(details);
    }
    return 1;
  }
  g_ubi_current_script_path = script_path;
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
  g_ubi_current_script_path.clear();
  return rc;
}

int UbiRunScriptFile(napi_env env, const char* script_path, std::string* error_out) {
  return UbiRunScriptFileWithLoop(env, script_path, error_out, false);
}

void UbiSetScriptArgv(const std::vector<std::string>& script_argv) {
  g_ubi_script_argv = script_argv;
}

void UbiSetExecArgv(const std::vector<std::string>& exec_argv) {
  g_ubi_cli_exec_argv = exec_argv;
}
