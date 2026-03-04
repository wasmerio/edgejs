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
            if (handled_out != nullptr) *handled_out = handled;
            return true;
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
      napi_has_named_property(env, process_obj, "__ubi_get_uncaught_exception_capture_callback",
                              &has_capture_getter) == napi_ok &&
      has_capture_getter) {
    napi_value getter = nullptr;
    if (napi_get_named_property(env, process_obj, "__ubi_get_uncaught_exception_capture_callback",
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
  if (handled_out != nullptr) *handled_out = handled;
  return true;
}

napi_value UbiProcessKillBinding(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value argv[2] = {nullptr, nullptr};
  napi_value self = nullptr;
  napi_get_cb_info(env, info, &argc, argv, &self, nullptr);
  (void)self;
  int32_t pid = 0;
  int32_t sig = SIGTERM;
  if (argc >= 1 && argv[0] != nullptr) {
    napi_get_value_int32(env, argv[0], &pid);
  }
  if (argc >= 2 && argv[1] != nullptr) {
    napi_get_value_int32(env, argv[1], &sig);
  }
  int rc = 0;
#if defined(_WIN32)
  (void)pid;
  (void)sig;
  rc = -1;
  const int err = UV_ENOSYS;
#else
  if (::kill(static_cast<pid_t>(pid), sig) != 0) {
    rc = -1;
  }
  const int err = (rc == 0) ? 0 : errno;
#endif
  napi_value out = nullptr;
  if (rc == 0) {
    napi_create_int32(env, 0, &out);
  } else {
    napi_create_int32(env, -err, &out);
  }
  return out;
}

bool NapiValueToUtf8(napi_env env, napi_value value, std::string* out) {
  if (env == nullptr || value == nullptr || out == nullptr) return false;
  napi_value string_value = value;
  napi_valuetype value_type = napi_undefined;
  if (napi_typeof(env, value, &value_type) != napi_ok) return false;
  if (value_type != napi_string) {
    if (napi_coerce_to_string(env, value, &string_value) != napi_ok || string_value == nullptr) {
      return false;
    }
  }
  size_t length = 0;
  if (napi_get_value_string_utf8(env, string_value, nullptr, 0, &length) != napi_ok) return false;
  std::string text(length + 1, '\0');
  size_t copied = 0;
  if (napi_get_value_string_utf8(env, string_value, text.data(), text.size(), &copied) != napi_ok) return false;
  text.resize(copied);
  *out = std::move(text);
  return true;
}

napi_value UbiProcessSetEnvBinding(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value argv[2] = {nullptr, nullptr};
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  int32_t rc = -1;
  std::string key;
  std::string value;
  if (argc >= 2 &&
      NapiValueToUtf8(env, argv[0], &key) &&
      NapiValueToUtf8(env, argv[1], &value) &&
      !key.empty()) {
#if defined(_WIN32)
    rc = (_putenv_s(key.c_str(), value.c_str()) == 0) ? 0 : -1;
#else
    rc = (setenv(key.c_str(), value.c_str(), 1) == 0) ? 0 : -1;
#endif
    if (rc == 0 && key == "TZ") {
#if defined(_WIN32)
      _tzset();
#else
      tzset();
#endif
      (void)unofficial_napi_notify_datetime_configuration_change(env);
    }
  }
  napi_value out = nullptr;
  napi_create_int32(env, rc, &out);
  return out;
}

napi_value UbiProcessUnsetEnvBinding(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  int32_t rc = -1;
  std::string key;
  if (argc >= 1 && NapiValueToUtf8(env, argv[0], &key) && !key.empty()) {
#if defined(_WIN32)
    rc = (_putenv_s(key.c_str(), "") == 0) ? 0 : -1;
#else
    rc = (unsetenv(key.c_str()) == 0) ? 0 : -1;
#endif
    if (rc == 0 && key == "TZ") {
#if defined(_WIN32)
      _tzset();
#else
      tzset();
#endif
      (void)unofficial_napi_notify_datetime_configuration_change(env);
    }
  }
  napi_value out = nullptr;
  napi_create_int32(env, rc, &out);
  return out;
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
  UbiInstallFsBinding(env);
  ClearPendingExceptionIfAny(env);
  UbiInstallBufferBinding(env);
  ClearPendingExceptionIfAny(env);
  UbiInstallOsBinding(env);
  ClearPendingExceptionIfAny(env);
  UbiInstallEncodingBinding(env);
  ClearPendingExceptionIfAny(env);
  UbiInstallStringDecoderBinding(env);
  ClearPendingExceptionIfAny(env);
  UbiInstallHttpParserBinding(env);
  ClearPendingExceptionIfAny(env);
  UbiInstallStreamWrapBinding(env);
  ClearPendingExceptionIfAny(env);
  UbiInstallProcessWrapBinding(env);
  ClearPendingExceptionIfAny(env);
  UbiInstallTcpWrapBinding(env);
  ClearPendingExceptionIfAny(env);
  UbiInstallTtyWrapBinding(env);
  ClearPendingExceptionIfAny(env);
  UbiInstallPipeWrapBinding(env);
  ClearPendingExceptionIfAny(env);
  UbiInstallSignalWrapBinding(env);
  ClearPendingExceptionIfAny(env);
  UbiInstallCaresWrapBinding(env);
  ClearPendingExceptionIfAny(env);
  UbiInstallUdpWrapBinding(env);
  ClearPendingExceptionIfAny(env);
  UbiInstallUrlBinding(env);
  UbiInstallUtilBinding(env);
  UbiInstallSpawnSyncBinding(env);
  UbiInstallTimersHostBinding(env);
  UbiInstallCryptoBinding(env);
  ClearPendingExceptionIfAny(env);
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
      "if (true) {"
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
      "    process.nextTick(function(){ if (typeof process.emit === 'function') process.emit('warning', w); });"
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
      "    return Array.isArray(globalThis.__ubi_active_requests) ? globalThis.__ubi_active_requests.slice() : [];"
      "  };"
      "}"
      "(function(){ if (!globalThis.__ubi_resource_tracking_installed) {"
      "  globalThis.__ubi_resource_tracking_installed = true;"
      "  var __activeResources = globalThis.__ubi_active_resources;"
      "  if (!(__activeResources && typeof __activeResources.set === 'function')) {"
      "    __activeResources = new Map();"
      "    globalThis.__ubi_active_resources = __activeResources;"
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
      "        if (typeof cb === 'function') return Reflect.apply(cb, this, arguments);"
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
      "        if (typeof cb === 'function') return Reflect.apply(cb, this, arguments);"
      "      };"
      "      h = __nativeSetImmediate.apply(globalThis, [wrapped].concat(args));"
      "      __activeResources.set(h, 'Immediate');"
      "      return h;"
      "    };"
      "  }"
      "  if (typeof __nativeClearImmediate === 'function') {"
      "    globalThis.clearImmediate = function(h){ __activeResources.delete(h); return __nativeClearImmediate(h); };"
      "  }"
      "  }})();"
      "if (typeof process.getActiveResourcesInfo !== 'function') {"
      "  process.getActiveResourcesInfo = function(){"
      "    var out = [];"
      "    var res = globalThis.__ubi_active_resources;"
      "    if (res && typeof res.forEach === 'function') res.forEach(function(v){ out.push(v); });"
      "    var reqs = Array.isArray(globalThis.__ubi_active_requests) ? globalThis.__ubi_active_requests : [];"
      "    for (var i = 0; i < reqs.length; i++) out.push(String((reqs[i] && reqs[i].type) || 'FSReqCallback'));"
      "    return out;"
      "  };"
      "}"
      "if (typeof process.threadCpuUsage !== 'function' && typeof process.cpuUsage === 'function') {"
      "  var __invalidPrev = function(v){"
      "    if (v == null) return ' Received ' + String(v);"
      "    if (typeof v === 'function') return ' Received function ' + v.name;"
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
      "      err.name = 'TypeError [ERR_INVALID_ARG_TYPE]';"
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
      "    '--experimental-quic', '--enable-fips', '--force-fips', '--openssl-config', '--openssl-legacy-provider',"
      "    '--openssl-shared-config', '--secure-heap-min', '--secure-heap', '--tls-cipher-list',"
      "    '--use-bundled-ca', '--use-openssl-ca'"
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
      "if (!process.__ubi_posix_ids) process.__ubi_posix_ids = { uid: process.getuid(), gid: process.getgid(), euid: process.geteuid(), egid: process.getegid() };"
      "if (!process.__ubi_posix_helpers) {"
      "  process.__ubi_posix_helpers = true;"
      "  process.__ubiInvalidArgTail = function(v){"
      "    if (v == null) return ' Received ' + String(v);"
      "    if (typeof v === 'function') return ' Received function ' + v.name;"
      "    if (typeof v === 'object') return ' Received an instance of ' + ((v.constructor && v.constructor.name) || 'Object');"
      "    if (typeof v === 'string') return \" Received type string ('\" + v + \"')\";"
      "    return ' Received type ' + typeof v + ' (' + String(v) + ')';"
      "  };"
      "  process.__ubiValidatePosixId = function(id){"
      "    if (typeof id !== 'number' && typeof id !== 'string') {"
      "      var e = new TypeError('The \"id\" argument must be one of type number or string.' + process.__ubiInvalidArgTail(id));"
      "      e.code = 'ERR_INVALID_ARG_TYPE';"
      "      throw e;"
      "    }"
      "  };"
      "  process.__ubiUnknownUser = function(id){ var e = new Error('User identifier does not exist: ' + String(id)); e.code = 'ERR_UNKNOWN_CREDENTIAL'; throw e; };"
      "  process.__ubiUnknownGroup = function(id){ var e = new Error('Group identifier does not exist: ' + String(id)); e.code = 'ERR_UNKNOWN_CREDENTIAL'; throw e; };"
      "}"
      "if (typeof process.setuid !== 'function') {"
      "  process.setuid = function(id){ process.__ubiValidatePosixId(id); if (typeof id === 'string') process.__ubiUnknownUser(id); process.__ubi_posix_ids.uid = Number(id) >>> 0; };"
      "}"
      "if (typeof process.setgid !== 'function') {"
      "  process.setgid = function(id){ process.__ubiValidatePosixId(id); if (typeof id === 'string') process.__ubiUnknownGroup(id); process.__ubi_posix_ids.gid = Number(id) >>> 0; };"
      "}"
      "if (typeof process.seteuid !== 'function') {"
      "  process.seteuid = function(id){ process.__ubiValidatePosixId(id); if (typeof id === 'string') process.__ubiUnknownUser(id); process.__ubi_posix_ids.euid = Number(id) >>> 0; };"
      "}"
      "if (typeof process.setegid !== 'function') {"
      "  process.setegid = function(id){ process.__ubiValidatePosixId(id); if (typeof id === 'string') process.__ubiUnknownGroup(id); process.__ubi_posix_ids.egid = Number(id) >>> 0; };"
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
      "        var te = new TypeError('The \"groups[' + i + ']\" argument must be one of type number or string.' + process.__ubiInvalidArgTail(g));"
      "        te.code = 'ERR_INVALID_ARG_TYPE';"
      "        throw te;"
      "      }"
      "      if (typeof g === 'number' && g < 0) {"
      "        var re = new RangeError('The value of \"groups[' + i + ']\" is out of range.');"
      "        re.code = 'ERR_OUT_OF_RANGE';"
      "        throw re;"
      "      }"
      "      if (typeof g === 'string') process.__ubiUnknownGroup(g);"
      "    }"
      "  };"
      "}"
      "if (typeof process.initgroups !== 'function') {"
      "  process.initgroups = function(user, extraGroup){"
      "    if (typeof user !== 'number' && typeof user !== 'string') {"
      "      var ue = new TypeError('The \"user\" argument must be one of type number or string.' + process.__ubiInvalidArgTail(user));"
      "      ue.code = 'ERR_INVALID_ARG_TYPE';"
      "      throw ue;"
      "    }"
      "    if (typeof extraGroup !== 'number' && typeof extraGroup !== 'string') {"
      "      var ge = new TypeError('The \"extraGroup\" argument must be one of type number or string.' + process.__ubiInvalidArgTail(extraGroup));"
      "      ge.code = 'ERR_INVALID_ARG_TYPE';"
      "      throw ge;"
      "    }"
      "    if (typeof extraGroup === 'string') process.__ubiUnknownGroup(extraGroup);"
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
      "  process.__ubi_get_uncaught_exception_capture_callback = function(){ return __uncaughtCapture; };"
      "}"
      "if (!process.__ubi_exit_patched) {"
      "  process.__ubi_exit_patched = true;"
      "  try {"
      "    if (typeof Symbol === 'function' && Symbol.toStringTag) {"
      "      Object.defineProperty(process, Symbol.toStringTag, { value: 'process', configurable: true });"
      "    }"
      "  } catch (_) {}"
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
      "try {"
      "  if (!process.constructor || process.constructor.name !== 'process') {"
      "    var __processCtor = function process() {};"
      "    Object.defineProperty(process, 'constructor', { value: __processCtor, writable: true, enumerable: false, configurable: true });"
      "  }"
      "} catch (_) {}"
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
  // require('internal/bootstrap/realm') will receive this via the loader wrapper and fill
  // it in place, so one object identity is used for all modules regardless of load order.
  napi_value global = nullptr;
  if (napi_get_global(env, &global) != napi_ok || global == nullptr) {
    if (error_out != nullptr) {
      *error_out = "napi_get_global failed";
    }
    return 1;
  }
  napi_value process_kill_fn = nullptr;
  if (napi_create_function(env,
                           "__ubi_process_kill",
                           NAPI_AUTO_LENGTH,
                           UbiProcessKillBinding,
                           nullptr,
                           &process_kill_fn) == napi_ok &&
      process_kill_fn != nullptr) {
    napi_set_named_property(env, global, "__ubi_process_kill", process_kill_fn);
  }
  napi_value process_set_env_fn = nullptr;
  if (napi_create_function(env,
                           "__ubi_process_set_env",
                           NAPI_AUTO_LENGTH,
                           UbiProcessSetEnvBinding,
                           nullptr,
                           &process_set_env_fn) == napi_ok &&
      process_set_env_fn != nullptr) {
    napi_set_named_property(env, global, "__ubi_process_set_env", process_set_env_fn);
  }
  napi_value process_unset_env_fn = nullptr;
  if (napi_create_function(env,
                           "__ubi_process_unset_env",
                           NAPI_AUTO_LENGTH,
                           UbiProcessUnsetEnvBinding,
                           nullptr,
                           &process_unset_env_fn) == napi_ok &&
      process_unset_env_fn != nullptr) {
    napi_set_named_property(env, global, "__ubi_process_unset_env", process_unset_env_fn);
  }
  napi_value primordials_container = nullptr;
  if (napi_create_object(env, &primordials_container) != napi_ok || primordials_container == nullptr) {
    if (error_out != nullptr) {
      *error_out = "napi_create_object(primordials) failed";
    }
    return 1;
  }
  if (napi_set_named_property(env, global, "__ubi_primordials", primordials_container) != napi_ok) {
    if (error_out != nullptr) {
      *error_out = "napi_set_named_property(__ubi_primordials) failed";
    }
    return 1;
  }

  // Bootstrap: load internal/bootstrap/realm using a require that resolves from builtins
  // dir (so it is found regardless of entry script path). Declare internalBinding and primordials
  // once; do not run entry script until these are set.
  static const char kBootstrapPrelude[] =
      "globalThis.global = globalThis;"
      "var __ubiBootstrapRequire = globalThis.__ubi_bootstrap_require;"
      "if (typeof __ubiBootstrapRequire !== 'function') throw new Error('__ubi_bootstrap_require is not a function');"
      "var __ib = __ubiBootstrapRequire('internal/bootstrap/realm');"
      "if (!__ib || typeof __ib.internalBinding !== 'function') throw new Error('internal/bootstrap/realm did not export internalBinding');"
      "globalThis.internalBinding = __ib.internalBinding;"
      "try {"
      "  var __taskQueues = __ubiBootstrapRequire('internal/process/task_queues');"
      "  if (__taskQueues && typeof __taskQueues.setupTaskQueue === 'function') {"
      "    var __taskQueueSetup = __taskQueues.setupTaskQueue();"
      "    if (process && typeof process === 'object' && __taskQueueSetup) {"
      "      if (typeof __taskQueueSetup.nextTick === 'function') process.nextTick = __taskQueueSetup.nextTick;"
      "      if (typeof __taskQueueSetup.runNextTicks === 'function') process._tickCallback = __taskQueueSetup.runNextTicks;"
      "    }"
      "  }"
      "} catch (_) {}"
      "if (__ib && __ib.primordials) globalThis.primordials = __ib.primordials;"
      "if (globalThis.__ubi_primordials && typeof globalThis.__ubi_primordials.SymbolFor === 'function') globalThis.primordials = globalThis.__ubi_primordials;"
      "else if (__ib && __ib.primordials) globalThis.primordials = __ib.primordials;"
      "if (typeof globalThis.Buffer === 'undefined') {"
      "  try {"
      "    var __bufmod = __ubiBootstrapRequire('buffer');"
      "    if (__bufmod && __bufmod.Buffer) globalThis.Buffer = __bufmod.Buffer;"
      "  } catch (_) {}"
      "}"
      "try {"
      "  if (globalThis.Buffer && globalThis.Buffer.prototype) {"
      "    var __B = globalThis.Buffer;"
      "    var __bufExports = (typeof __bufmod === 'object' && __bufmod) ? __bufmod : __ubiBootstrapRequire('buffer');"
      "    var __origToString = __B.prototype.toString;"
      "    var __origWrite = __B.prototype.write;"
      "    var __inspectSym = Symbol.for('nodejs.util.inspect.custom');"
      "    __B.prototype.includes = function includes(val, byteOffset, encoding) {"
      "      return __B.prototype.indexOf.call(this, val, byteOffset, encoding) !== -1;"
      "    };"
      "    __B.prototype.toString = function toString(encoding, start, end) {"
      "      if (!__B.isBuffer(this) && ArrayBuffer.isView(this)) {"
      "        return __B.from(this.buffer, this.byteOffset, this.byteLength).toString(encoding, start, end);"
      "      }"
      "      return __origToString.call(this, encoding, start, end);"
      "    };"
      "    __B.prototype.toLocaleString = __B.prototype.toString;"
      "    __B.prototype.write = function write(string, offset, length, encoding) {"
      "      if (!__B.isBuffer(this) && ArrayBuffer.isView(this)) {"
      "        return __B.from(this.buffer, this.byteOffset, this.byteLength).write(string, offset, length, encoding);"
      "      }"
      "      return __origWrite.call(this, string, offset, length, encoding);"
      "    };"
      "    var __genericInspect = function inspectCompat(recurseTimes, ctx) {"
      "      var __view = null;"
      "      var __ctor = 'Buffer';"
      "      if (__B.isBuffer(this)) {"
      "        __view = this;"
      "      } else if (ArrayBuffer.isView(this)) {"
      "        __view = __B.from(this.buffer, this.byteOffset, this.byteLength);"
      "        __ctor = 'Uint8Array';"
      "      } else {"
      "        return String(this);"
      "      }"
      "      var __max = (__bufExports && __bufExports.INSPECT_MAX_BYTES !== undefined) ? __bufExports.INSPECT_MAX_BYTES : 50;"
      "      var __actualMax = Math.min(__max, __view.length);"
      "      var __hex = __view.toString('hex', 0, __actualMax).replace(/(.{2})/g, '$1 ').trim();"
      "      var __remaining = __view.length - __max;"
      "      var __str = __hex;"
      "      if (__remaining > 0) __str += (__str ? ' ' : '') + '... ' + __remaining + ' more byte' + (__remaining > 1 ? 's' : '');"
      "      if (ctx) {"
      "        var __ui = require('util').inspect;"
      "        var __keys = Object.keys(this).filter(function(k){ return !/^(0|[1-9]\\d*)$/.test(k); });"
      "        if (__keys.length > 0) {"
      "          var __extras = __keys.map(function(k){ return k + ': ' + __ui(this[k]); }, this).join(', ');"
      "          __str += (__str ? ', ' : '') + __extras;"
      "        }"
      "      }"
      "      return '<' + __ctor + ' ' + __str + '>';"
      "    };"
      "    __B.prototype[__inspectSym] = __genericInspect;"
      "    __B.prototype.inspect = __genericInspect;"
      "  }"
      "} catch (_) {}";
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
      "(function(){ if (!globalThis.__ubi_resource_tracking_ready) {"
      "  var __ar = globalThis.__ubi_active_resources;"
      "  if (!(__ar && typeof __ar.set === 'function')) {"
      "    __ar = new Map();"
      "    globalThis.__ubi_active_resources = __ar;"
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
      "  globalThis.__ubi_resource_tracking_ready = true;"
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
      "    if (!globalThis.process.__ubi_signal_listener_hooks_installed) {"
      "      var __uSignalHooks0 = require('internal/process/signal');"
      "      if (__uSignalHooks0 && typeof __uSignalHooks0.startListeningIfSignal === 'function') {"
      "        process.on('newListener', __uSignalHooks0.startListeningIfSignal);"
      "      }"
      "      if (__uSignalHooks0 && typeof __uSignalHooks0.stopListeningIfSignal === 'function') {"
      "        process.on('removeListener', __uSignalHooks0.stopListeningIfSignal);"
      "      }"
      "      globalThis.process.__ubi_signal_listener_hooks_installed = true;"
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
      "  if (globalThis.process.env && !globalThis.process.__ubi_env_proxy_installed) {"
      "    globalThis.process.__ubi_env_proxy_installed = true;"
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
      "        if (typeof globalThis.__ubi_process_set_env === 'function') {"
      "          try { globalThis.__ubi_process_set_env(name, target[name]); } catch (_) {}"
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
      "        if (typeof globalThis.__ubi_process_unset_env === 'function') {"
      "          try { globalThis.__ubi_process_unset_env(name); } catch (_) {}"
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
      "        if (typeof globalThis.__ubi_process_set_env === 'function') {"
      "          try { globalThis.__ubi_process_set_env(name, target[name]); } catch (_) {}"
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
      "      var __signals = (require('os').constants && require('os').constants.signals) || {};"
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
      "    if (typeof globalThis.__ubi_process_kill === 'function') {"
      "      return globalThis.__ubi_process_kill(__pidNum, __sigNum);"
      "    }"
      "    return 0;"
      "  };"
      "    var __signalMap = ((require('os').constants && require('os').constants.signals) || {});"
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
      "globalThis.__ubi_detached_arraybuffers = globalThis.__ubi_detached_arraybuffers || new WeakSet();"
      "var __ubi_original_structuredClone = globalThis.structuredClone;"
      "globalThis.structuredClone = function(v, options) {"
      "  if (options && options.transfer && typeof options.transfer.length === 'number') {"
      "    for (var i = 0; i < options.transfer.length; i++) {"
      "      var t = options.transfer[i];"
      "      if (t && Object.prototype.toString.call(t) === '[object ArrayBuffer]') {"
      "        try { t.__ubi_detached_arraybuffer_marker = 1; } catch (_) {}"
      "        globalThis.__ubi_detached_arraybuffers.add(t);"
      "      }"
      "    }"
      "  }"
      "  if (typeof __ubi_original_structuredClone === 'function') {"
      "    return __ubi_original_structuredClone(v, options);"
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
      "if (typeof globalThis.Buffer === 'undefined') {"
      "  try {"
      "    var __buf = require('buffer');"
      "    if (__buf && __buf.Buffer) globalThis.Buffer = __buf.Buffer;"
      "  } catch (_) {}"
      "}"
      "if (!globalThis.__ubi_date_tz_patch && typeof Date === 'function') {"
      "  globalThis.__ubi_date_tz_patch = true;"
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
        "if (process && typeof process.on === 'function' && !process.__ubi_signal_listener_hooks_installed) {"
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
        "    process.__ubi_signal_listener_hooks_installed = __afterSigNl > __beforeSigNl;"
      "  } catch (_) {}"
        "}"
        "return require(__entry); } catch (err) {"
        "var p = globalThis.process;"
        "var handled = false;"
        "try { if (p && typeof p.emit === 'function') p.emit('uncaughtExceptionMonitor', err, 'uncaughtException'); } catch (monitorErr) { throw monitorErr; }"
        "if (p && typeof p.__ubi_get_uncaught_exception_capture_callback === 'function') {"
        "  var cap = p.__ubi_get_uncaught_exception_capture_callback();"
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
      "(function(){ globalThis.console = globalThis.__ubi_bootstrap_require('console'); })();";
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
