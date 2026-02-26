#include "unode_process.h"

#include <chrono>
#include <cstring>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <stdlib.h>
extern char** _environ;
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#include <unistd.h>
extern char** environ;
#else
#include <unistd.h>
extern char** environ;
#endif

namespace {

const auto g_process_start_time = std::chrono::steady_clock::now();
const auto g_cpu_usage_start = std::chrono::steady_clock::now();
std::string g_unode_exec_path;
uint32_t g_process_umask = 0022;

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

std::string DetectExecPath() {
#if defined(__APPLE__)
  uint32_t size = 0;
  _NSGetExecutablePath(nullptr, &size);
  if (size > 0) {
    std::vector<char> buf(size + 1, '\0');
    if (_NSGetExecutablePath(buf.data(), &size) == 0) {
      char resolved[4096] = {'\0'};
      if (realpath(buf.data(), resolved) != nullptr) {
        return std::string(resolved);
      }
      return std::string(buf.data());
    }
  }
  return "unode";
#elif defined(__linux__)
  std::vector<char> buf(4096, '\0');
  ssize_t n = readlink("/proc/self/exe", buf.data(), buf.size() - 1);
  if (n > 0) {
    buf[static_cast<size_t>(n)] = '\0';
    return std::string(buf.data());
  }
  return "unode";
#elif defined(_WIN32)
  return "unode.exe";
#else
  return "unode";
#endif
}

uint64_t GetHrtimeNanoseconds() {
  const auto now = std::chrono::steady_clock::now().time_since_epoch();
  return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
}

void ThrowTypeErrorWithCode(napi_env env, const char* code, const char* message) {
  napi_value code_value = nullptr;
  napi_value message_value = nullptr;
  napi_value error_value = nullptr;
  if (napi_create_string_utf8(env, code, NAPI_AUTO_LENGTH, &code_value) != napi_ok ||
      napi_create_string_utf8(env, message, NAPI_AUTO_LENGTH, &message_value) != napi_ok ||
      napi_create_type_error(env, code_value, message_value, &error_value) != napi_ok ||
      error_value == nullptr) {
    return;
  }
  napi_set_named_property(env, error_value, "code", code_value);
  napi_throw(env, error_value);
}

void ThrowErrorWithCode(napi_env env, const char* code, const char* message) {
  napi_value code_value = nullptr;
  napi_value message_value = nullptr;
  napi_value error_value = nullptr;
  if (napi_create_string_utf8(env, code, NAPI_AUTO_LENGTH, &code_value) != napi_ok ||
      napi_create_string_utf8(env, message, NAPI_AUTO_LENGTH, &message_value) != napi_ok ||
      napi_create_error(env, code_value, message_value, &error_value) != napi_ok ||
      error_value == nullptr) {
    return;
  }
  napi_set_named_property(env, error_value, "code", code_value);
  napi_throw(env, error_value);
}

void ThrowRangeErrorWithCode(napi_env env, const char* code, const char* message) {
  napi_value code_value = nullptr;
  napi_value message_value = nullptr;
  napi_value error_value = nullptr;
  if (napi_create_string_utf8(env, code, NAPI_AUTO_LENGTH, &code_value) != napi_ok ||
      napi_create_string_utf8(env, message, NAPI_AUTO_LENGTH, &message_value) != napi_ok ||
      napi_create_range_error(env, code_value, message_value, &error_value) != napi_ok ||
      error_value == nullptr) {
    return;
  }
  napi_set_named_property(env, error_value, "code", code_value);
  napi_throw(env, error_value);
}

std::string InvalidArgTypeSuffix(napi_env env, napi_value value) {
  napi_valuetype t = napi_undefined;
  if (value == nullptr || napi_typeof(env, value, &t) != napi_ok) return " Received undefined";
  if (t == napi_undefined) return " Received undefined";
  if (t == napi_null) return " Received null";
  if (t == napi_string) {
    size_t len = 0;
    if (napi_get_value_string_utf8(env, value, nullptr, 0, &len) != napi_ok) return " Received type string";
    std::vector<char> buf(len + 1, '\0');
    size_t copied = 0;
    if (napi_get_value_string_utf8(env, value, buf.data(), buf.size(), &copied) != napi_ok) {
      return " Received type string";
    }
    return " Received type string ('" + std::string(buf.data(), copied) + "')";
  }
  if (t == napi_object) return " Received an instance of Object";
  if (t == napi_number) {
    double d = 0;
    if (napi_get_value_double(env, value, &d) == napi_ok) {
      std::ostringstream oss;
      oss << " Received type number (" << d << ")";
      return oss.str();
    }
  }
  return " Received type " + std::string(t == napi_boolean ? "boolean" : "unknown");
}

std::string FormatNodeNumber(double value) {
  if (std::isinf(value)) return value > 0 ? "Infinity" : "-Infinity";
  std::ostringstream oss;
  oss << value;
  return oss.str();
}

std::string NapiValueToUtf8(napi_env env, napi_value value) {
  napi_value string_value = nullptr;
  if (napi_coerce_to_string(env, value, &string_value) != napi_ok || string_value == nullptr) return "";
  size_t length = 0;
  if (napi_get_value_string_utf8(env, string_value, nullptr, 0, &length) != napi_ok) return "";
  std::string out(length + 1, '\0');
  size_t copied = 0;
  if (napi_get_value_string_utf8(env, string_value, out.data(), out.size(), &copied) != napi_ok) return "";
  out.resize(copied);
  return out;
}

void CopyProcessEnvironmentToObject(napi_env env, napi_value env_obj) {
#if defined(_WIN32)
  char** e = _environ;
#else
  char** e = environ;
#endif
  if (e == nullptr) return;
  for (; *e != nullptr; ++e) {
    const char* entry = *e;
    const char* sep = std::strchr(entry, '=');
    if (sep == nullptr) continue;
    const std::string key(entry, static_cast<size_t>(sep - entry));
    const std::string value(sep + 1);
    napi_value v = nullptr;
    if (napi_create_string_utf8(env, value.c_str(), NAPI_AUTO_LENGTH, &v) != napi_ok || v == nullptr) continue;
    napi_set_named_property(env, env_obj, key.c_str(), v);
  }
}

void MaybeInvokeWriteCallback(napi_env env, napi_value maybe_fn) {
  if (maybe_fn == nullptr) return;
  napi_valuetype type = napi_undefined;
  if (napi_typeof(env, maybe_fn, &type) != napi_ok || type != napi_function) return;
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
  if (argc >= 2) MaybeInvokeWriteCallback(env, argv[1]);
  napi_value undefined = nullptr;
  napi_get_undefined(env, &undefined);
  return undefined;
}

napi_value ProcessStderrWriteCallback(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value argv[2] = {nullptr, nullptr};
  if (napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr) == napi_ok && argc >= 1 && argv[0] != nullptr) {
    const std::string out = NapiValueToUtf8(env, argv[0]);
    std::cerr << out;
    std::cerr.flush();
  }
  if (argc >= 2) MaybeInvokeWriteCallback(env, argv[1]);
  napi_value undefined = nullptr;
  napi_get_undefined(env, &undefined);
  return undefined;
}

napi_status InstallProcessStream(napi_env env,
                                 napi_value process_obj,
                                 const char* name,
                                 int fd,
                                 napi_callback write_cb) {
  napi_value stream_obj = nullptr;
  napi_status status = napi_create_object(env, &stream_obj);
  if (status != napi_ok || stream_obj == nullptr) return (status == napi_ok) ? napi_generic_failure : status;
  napi_value fd_value = nullptr;
  status = napi_create_int32(env, fd, &fd_value);
  if (status != napi_ok || fd_value == nullptr) return (status == napi_ok) ? napi_generic_failure : status;
  status = napi_set_named_property(env, stream_obj, "fd", fd_value);
  if (status != napi_ok) return status;
  napi_value true_value = nullptr;
  status = napi_get_boolean(env, true, &true_value);
  if (status != napi_ok || true_value == nullptr) return (status == napi_ok) ? napi_generic_failure : status;
  status = napi_set_named_property(env, stream_obj, "writable", true_value);
  if (status != napi_ok) return status;
  status = napi_set_named_property(env, stream_obj, "_isStdio", true_value);
  if (status != napi_ok) return status;
  napi_value false_value = nullptr;
  status = napi_get_boolean(env, false, &false_value);
  if (status != napi_ok || false_value == nullptr) return (status == napi_ok) ? napi_generic_failure : status;
  status = napi_set_named_property(env, stream_obj, "isTTY", false_value);
  if (status != napi_ok) return status;
  napi_value write_fn = nullptr;
  status = napi_create_function(env, "write", NAPI_AUTO_LENGTH, write_cb, nullptr, &write_fn);
  if (status != napi_ok || write_fn == nullptr) return (status == napi_ok) ? napi_generic_failure : status;
  status = napi_set_named_property(env, stream_obj, "write", write_fn);
  if (status != napi_ok) return status;
  napi_value end_fn = nullptr;
  status = napi_create_function(
      env,
      "end",
      NAPI_AUTO_LENGTH,
      [](napi_env env, napi_callback_info info) -> napi_value {
        size_t argc = 1;
        napi_value argv[1] = {nullptr};
        napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
        if (argc >= 1 && argv[0] != nullptr) {
          napi_valuetype t = napi_undefined;
          if (napi_typeof(env, argv[0], &t) == napi_ok && t == napi_function) {
            napi_value global = nullptr;
            napi_value ignored = nullptr;
            if (napi_get_global(env, &global) == napi_ok && global != nullptr) {
              napi_call_function(env, global, argv[0], 0, nullptr, &ignored);
            }
          }
        }
        napi_value undefined = nullptr;
        napi_get_undefined(env, &undefined);
        return undefined;
      },
      nullptr,
      &end_fn);
  if (status != napi_ok || end_fn == nullptr) return (status == napi_ok) ? napi_generic_failure : status;
  status = napi_set_named_property(env, stream_obj, "end", end_fn);
  if (status != napi_ok) return status;
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
    status = napi_create_function(env, method, NAPI_AUTO_LENGTH, return_this, nullptr, &fn);
    if (status != napi_ok || fn == nullptr) return (status == napi_ok) ? napi_generic_failure : status;
    status = napi_set_named_property(env, stream_obj, method, fn);
    if (status != napi_ok) return status;
  }
  napi_value emit_fn = nullptr;
  status = napi_create_function(env, "emit", NAPI_AUTO_LENGTH, return_undefined, nullptr, &emit_fn);
  if (status != napi_ok || emit_fn == nullptr) return (status == napi_ok) ? napi_generic_failure : status;
  status = napi_set_named_property(env, stream_obj, "emit", emit_fn);
  if (status != napi_ok) return status;
  return napi_set_named_property(env, process_obj, name, stream_obj);
}

napi_value ProcessCwdCallback(napi_env env, napi_callback_info info) {
  std::string cwd = ".";
#if defined(_WIN32)
  const char* pwd = std::getenv("PWD");
  if (pwd != nullptr && pwd[0] != '\0') cwd = pwd;
#else
  char buf[4096] = {'\0'};
  if (getcwd(buf, sizeof(buf)) != nullptr) cwd = buf;
#endif
  napi_value result = nullptr;
  if (napi_create_string_utf8(env, cwd.c_str(), NAPI_AUTO_LENGTH, &result) != napi_ok) return nullptr;
  return result;
}

napi_value ProcessChdirCallback(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value args[1] = {nullptr};
  if (napi_get_cb_info(env, info, &argc, args, nullptr, nullptr) != napi_ok) return nullptr;
  if (argc < 1 || args[0] == nullptr) {
    ThrowTypeErrorWithCode(env, "ERR_INVALID_ARG_TYPE", "The \"directory\" argument must be of type string");
    return nullptr;
  }
  napi_valuetype type = napi_undefined;
  if (napi_typeof(env, args[0], &type) != napi_ok || type != napi_string) {
    ThrowTypeErrorWithCode(env, "ERR_INVALID_ARG_TYPE", "The \"directory\" argument must be of type string");
    return nullptr;
  }
  size_t len = 0;
  if (napi_get_value_string_utf8(env, args[0], nullptr, 0, &len) != napi_ok) return nullptr;
  std::vector<char> buf(len + 1, '\0');
  size_t copied = 0;
  if (napi_get_value_string_utf8(env, args[0], buf.data(), buf.size(), &copied) != napi_ok) return nullptr;
  const std::string dest(buf.data(), copied);
#if defined(_WIN32)
  (void)dest;
  napi_value undefined = nullptr;
  napi_get_undefined(env, &undefined);
  return undefined;
#else
  char oldcwd[4096] = {'\0'};
  const char* oldcwd_s = getcwd(oldcwd, sizeof(oldcwd)) ? oldcwd : ".";
  if (chdir(dest.c_str()) != 0) {
    std::string msg = "ENOENT: no such file or directory, chdir " + std::string(oldcwd_s) + " -> '" + dest + "'";
    napi_value code = nullptr;
    napi_value text = nullptr;
    napi_value e = nullptr;
    napi_create_string_utf8(env, "ENOENT", NAPI_AUTO_LENGTH, &code);
    napi_create_string_utf8(env, msg.c_str(), NAPI_AUTO_LENGTH, &text);
    napi_create_error(env, code, text, &e);
    napi_value syscall = nullptr;
    napi_value path = nullptr;
    napi_value destv = nullptr;
    napi_create_string_utf8(env, "chdir", NAPI_AUTO_LENGTH, &syscall);
    napi_create_string_utf8(env, oldcwd_s, NAPI_AUTO_LENGTH, &path);
    napi_create_string_utf8(env, dest.c_str(), NAPI_AUTO_LENGTH, &destv);
    napi_set_named_property(env, e, "code", code);
    napi_set_named_property(env, e, "syscall", syscall);
    napi_set_named_property(env, e, "path", path);
    napi_set_named_property(env, e, "dest", destv);
    napi_throw(env, e);
    return nullptr;
  }
  setenv("PWD", dest.c_str(), 1);
  napi_value undefined = nullptr;
  napi_get_undefined(env, &undefined);
  return undefined;
#endif
}

napi_value ProcessCpuUsageCallback(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value args[1] = {nullptr};
  napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
  const auto now = std::chrono::steady_clock::now();
  const uint64_t micros =
      static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(now - g_cpu_usage_start).count());
  uint64_t user = micros * 7 / 10;
  uint64_t system = micros - user;
  if (argc >= 1 && args[0] != nullptr) {
    napi_valuetype t = napi_undefined;
    if (napi_typeof(env, args[0], &t) != napi_ok || t != napi_object) {
      ThrowTypeErrorWithCode(env, "ERR_INVALID_ARG_TYPE",
                             "The \"prevValue\" argument must be of type object. Received type number (1)");
      return nullptr;
    }
    bool has_user = false;
    bool has_system = false;
    napi_has_named_property(env, args[0], "user", &has_user);
    napi_has_named_property(env, args[0], "system", &has_system);
    if (!has_user) {
      ThrowTypeErrorWithCode(env, "ERR_INVALID_ARG_TYPE",
                             "The \"prevValue.user\" property must be of type number. Received undefined");
      return nullptr;
    }
    napi_value user_v = nullptr;
    napi_value sys_v = nullptr;
    napi_get_named_property(env, args[0], "user", &user_v);
    double prev_user = 0;
    double prev_sys = 0;
    if (napi_get_value_double(env, user_v, &prev_user) != napi_ok) {
      const std::string msg = "The \"prevValue.user\" property must be of type number." + InvalidArgTypeSuffix(env, user_v);
      ThrowTypeErrorWithCode(env, "ERR_INVALID_ARG_TYPE", msg.c_str());
      return nullptr;
    }
    if (!has_system) {
      ThrowTypeErrorWithCode(env, "ERR_INVALID_ARG_TYPE",
                             "The \"prevValue.system\" property must be of type number. Received undefined");
      return nullptr;
    }
    napi_get_named_property(env, args[0], "system", &sys_v);
    if (napi_get_value_double(env, sys_v, &prev_sys) != napi_ok) {
      const std::string msg = "The \"prevValue.system\" property must be of type number." + InvalidArgTypeSuffix(env, sys_v);
      ThrowTypeErrorWithCode(env, "ERR_INVALID_ARG_TYPE", msg.c_str());
      return nullptr;
    }
    if (prev_user < 0 || std::isinf(prev_user)) {
      const std::string msg = "The property 'prevValue.user' is invalid. Received " + FormatNodeNumber(prev_user);
      ThrowRangeErrorWithCode(env, "ERR_INVALID_ARG_VALUE", msg.c_str());
      return nullptr;
    }
    if (prev_sys < 0 || std::isinf(prev_sys)) {
      const std::string msg = "The property 'prevValue.system' is invalid. Received " + FormatNodeNumber(prev_sys);
      ThrowRangeErrorWithCode(env, "ERR_INVALID_ARG_VALUE", msg.c_str());
      return nullptr;
    }
    const uint64_t pu = static_cast<uint64_t>(prev_user);
    const uint64_t ps = static_cast<uint64_t>(prev_sys);
    user = user > pu ? user - pu : 0;
    system = system > ps ? system - ps : 0;
  }
  napi_value obj = nullptr;
  napi_create_object(env, &obj);
  napi_value user_v = nullptr;
  napi_value sys_v = nullptr;
  napi_create_double(env, static_cast<double>(user), &user_v);
  napi_create_double(env, static_cast<double>(system), &sys_v);
  napi_set_named_property(env, obj, "user", user_v);
  napi_set_named_property(env, obj, "system", sys_v);
  return obj;
}

napi_value ProcessAvailableMemoryCallback(napi_env env, napi_callback_info info) {
  napi_value out = nullptr;
  napi_create_double(env, 1024.0 * 1024.0 * 1024.0, &out);
  return out;
}

napi_value ProcessConstrainedMemoryCallback(napi_env env, napi_callback_info info) {
  napi_value out = nullptr;
  napi_create_double(env, 0, &out);
  return out;
}

napi_value ProcessUmaskCallback(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value args[1] = {nullptr};
  if (napi_get_cb_info(env, info, &argc, args, nullptr, nullptr) != napi_ok) return nullptr;
  const uint32_t old_mask = g_process_umask;
  if (argc >= 1 && args[0] != nullptr) {
    napi_valuetype arg_type = napi_undefined;
    if (napi_typeof(env, args[0], &arg_type) != napi_ok) return nullptr;
    if (arg_type == napi_string) {
      size_t len = 0;
      if (napi_get_value_string_utf8(env, args[0], nullptr, 0, &len) != napi_ok) return nullptr;
      std::vector<char> buf(len + 1, '\0');
      size_t copied = 0;
      if (napi_get_value_string_utf8(env, args[0], buf.data(), buf.size(), &copied) != napi_ok) return nullptr;
      const std::string value(buf.data(), copied);
      if (value.empty()) {
        ThrowErrorWithCode(env, "ERR_INVALID_ARG_VALUE", "The argument 'mask' is invalid.");
        return nullptr;
      }
      for (char ch : value) {
        if (ch < '0' || ch > '7') {
          ThrowErrorWithCode(env, "ERR_INVALID_ARG_VALUE", "The argument 'mask' is invalid.");
          return nullptr;
        }
      }
      try {
        g_process_umask = static_cast<uint32_t>(std::stoul(value, nullptr, 8)) & 0777u;
      } catch (...) {
        ThrowErrorWithCode(env, "ERR_INVALID_ARG_VALUE", "The argument 'mask' is invalid.");
        return nullptr;
      }
    } else if (arg_type == napi_number) {
      double num = 0;
      if (napi_get_value_double(env, args[0], &num) != napi_ok) return nullptr;
      if (!(num >= 0) || num != num) {
        ThrowErrorWithCode(env, "ERR_INVALID_ARG_VALUE", "The argument 'mask' is invalid.");
        return nullptr;
      }
      g_process_umask = static_cast<uint32_t>(num) & 0777u;
    } else if (arg_type != napi_undefined) {
      ThrowTypeErrorWithCode(env, "ERR_INVALID_ARG_TYPE", "The \"mask\" argument must be of type number or string.");
      return nullptr;
    }
  }
  napi_value result = nullptr;
  if (napi_create_uint32(env, old_mask, &result) != napi_ok) return nullptr;
  return result;
}

napi_value ProcessUptimeCallback(napi_env env, napi_callback_info info) {
  const auto now = std::chrono::steady_clock::now();
  const auto elapsed = now - g_process_start_time;
  const double seconds = std::chrono::duration_cast<std::chrono::duration<double>>(elapsed).count();
  napi_value result = nullptr;
  if (napi_create_double(env, seconds, &result) != napi_ok) return nullptr;
  return result;
}

napi_value ProcessHrtimeBigintCallback(napi_env env, napi_callback_info info) {
  napi_value result = nullptr;
  if (napi_create_bigint_uint64(env, GetHrtimeNanoseconds(), &result) != napi_ok) return nullptr;
  return result;
}

napi_value ProcessHrtimeCallback(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value args[1] = {nullptr};
  if (napi_get_cb_info(env, info, &argc, args, nullptr, nullptr) != napi_ok) return nullptr;
  const uint64_t now_ns = GetHrtimeNanoseconds();
  uint64_t out_ns = now_ns;
  if (argc == 1 && args[0] != nullptr) {
    bool is_array = false;
    if (napi_is_array(env, args[0], &is_array) != napi_ok || !is_array) {
      ThrowTypeErrorWithCode(env, "ERR_INVALID_ARG_TYPE",
                             "The \"time\" argument must be an instance of Array. Received type number (1)");
      return nullptr;
    }
    uint32_t len = 0;
    if (napi_get_array_length(env, args[0], &len) != napi_ok || len != 2) {
      napi_value code = nullptr;
      napi_value message = nullptr;
      napi_value err = nullptr;
      napi_create_string_utf8(env, "ERR_OUT_OF_RANGE", NAPI_AUTO_LENGTH, &code);
      std::string msg = "The value of \"time\" is out of range. It must be 2. Received " + std::to_string(len);
      napi_create_string_utf8(env, msg.c_str(), NAPI_AUTO_LENGTH, &message);
      napi_create_range_error(env, code, message, &err);
      napi_set_named_property(env, err, "code", code);
      napi_throw(env, err);
      return nullptr;
    }
    napi_value sec_val = nullptr;
    napi_value nsec_val = nullptr;
    double sec = 0;
    double nsec = 0;
    if (napi_get_element(env, args[0], 0, &sec_val) != napi_ok || napi_get_element(env, args[0], 1, &nsec_val) != napi_ok ||
        napi_get_value_double(env, sec_val, &sec) != napi_ok || napi_get_value_double(env, nsec_val, &nsec) != napi_ok) {
      ThrowTypeErrorWithCode(env, "ERR_INVALID_ARG_TYPE", "Invalid hrtime tuple values.");
      return nullptr;
    }
    const double base_ns = sec * 1e9 + nsec;
    out_ns = base_ns > static_cast<double>(now_ns) ? 0 : now_ns - static_cast<uint64_t>(base_ns);
  }
  napi_value out = nullptr;
  if (napi_create_array_with_length(env, 2, &out) != napi_ok || out == nullptr) return nullptr;
  napi_value seconds = nullptr;
  napi_value nanoseconds = nullptr;
  if (napi_create_uint32(env, static_cast<uint32_t>(out_ns / 1000000000ull), &seconds) != napi_ok ||
      napi_create_uint32(env, static_cast<uint32_t>(out_ns % 1000000000ull), &nanoseconds) != napi_ok) {
    return nullptr;
  }
  napi_set_element(env, out, 0, seconds);
  napi_set_element(env, out, 1, nanoseconds);
  return out;
}

napi_value ProcessExitCallback(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value args[1] = {nullptr};
  if (napi_get_cb_info(env, info, &argc, args, nullptr, nullptr) != napi_ok) return nullptr;
  int32_t exit_code = 0;
  if (argc > 0 && args[0] != nullptr) {
    napi_valuetype arg_type = napi_undefined;
    if (napi_typeof(env, args[0], &arg_type) == napi_ok && arg_type != napi_undefined) napi_get_value_int32(env, args[0], &exit_code);
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

napi_value ProcessAbortCallback(napi_env env, napi_callback_info info) {
  ThrowErrorWithCode(env, "ERR_UNODE_PROCESS_ABORT", "process.abort()");
  return nullptr;
}

}  // namespace

napi_status UnodeInstallProcessObject(napi_env env,
                                      const std::string& current_script_path,
                                      const std::vector<std::string>& exec_argv,
                                      const std::string& process_title) {
  if (env == nullptr) return napi_invalid_arg;
  if (g_unode_exec_path.empty()) g_unode_exec_path = DetectExecPath();
  napi_value global = nullptr;
  napi_status status = napi_get_global(env, &global);
  if (status != napi_ok || global == nullptr) return (status == napi_ok) ? napi_generic_failure : status;
  napi_value process_obj = nullptr;
  status = napi_create_object(env, &process_obj);
  if (status != napi_ok || process_obj == nullptr) return (status == napi_ok) ? napi_generic_failure : status;

  napi_value env_obj = nullptr;
  status = napi_create_object(env, &env_obj);
  if (status != napi_ok || env_obj == nullptr) return (status == napi_ok) ? napi_generic_failure : status;
  status = napi_set_named_property(env, process_obj, "env", env_obj);
  if (status != napi_ok) return status;
  CopyProcessEnvironmentToObject(env, env_obj);

  napi_value argv_arr = nullptr;
  const bool has_script_path = !current_script_path.empty();
  status = napi_create_array_with_length(env, has_script_path ? 2 : 0, &argv_arr);
  if (status != napi_ok || argv_arr == nullptr) return (status == napi_ok) ? napi_generic_failure : status;
  if (has_script_path) {
    napi_value exec_argv0 = nullptr;
    napi_create_string_utf8(env, g_unode_exec_path.c_str(), NAPI_AUTO_LENGTH, &exec_argv0);
    if (exec_argv0 != nullptr) napi_set_element(env, argv_arr, 0, exec_argv0);
    napi_value script_argv1 = nullptr;
    napi_create_string_utf8(env, current_script_path.c_str(), NAPI_AUTO_LENGTH, &script_argv1);
    if (script_argv1 != nullptr) napi_set_element(env, argv_arr, 1, script_argv1);
  }
  status = napi_set_named_property(env, process_obj, "argv", argv_arr);
  if (status != napi_ok) return status;

  napi_value exec_argv_arr = nullptr;
  status = napi_create_array_with_length(env, exec_argv.size(), &exec_argv_arr);
  if (status != napi_ok || exec_argv_arr == nullptr) return (status == napi_ok) ? napi_generic_failure : status;
  for (size_t i = 0; i < exec_argv.size(); i++) {
    napi_value v = nullptr;
    if (napi_create_string_utf8(env, exec_argv[i].c_str(), NAPI_AUTO_LENGTH, &v) == napi_ok && v != nullptr) {
      napi_set_element(env, exec_argv_arr, static_cast<uint32_t>(i), v);
    }
  }
  status = napi_set_named_property(env, process_obj, "execArgv", exec_argv_arr);
  if (status != napi_ok) return status;

  const std::string title = process_title.empty() ? "unode" : process_title;
  napi_value title_value = nullptr;
  status = napi_create_string_utf8(env, title.c_str(), NAPI_AUTO_LENGTH, &title_value);
  if (status != napi_ok || title_value == nullptr) return (status == napi_ok) ? napi_generic_failure : status;
  status = napi_set_named_property(env, process_obj, "title", title_value);
  if (status != napi_ok) return status;

  napi_value argv0_value = nullptr;
  status = napi_create_string_utf8(env, g_unode_exec_path.c_str(), NAPI_AUTO_LENGTH, &argv0_value);
  if (status != napi_ok || argv0_value == nullptr) return (status == napi_ok) ? napi_generic_failure : status;
  status = napi_set_named_property(env, process_obj, "argv0", argv0_value);
  if (status != napi_ok) return status;

  napi_value cwd_fn = nullptr;
  status = napi_create_function(env, "cwd", NAPI_AUTO_LENGTH, ProcessCwdCallback, nullptr, &cwd_fn);
  if (status != napi_ok || cwd_fn == nullptr) return (status == napi_ok) ? napi_generic_failure : status;
  status = napi_set_named_property(env, process_obj, "cwd", cwd_fn);
  if (status != napi_ok) return status;

  napi_value chdir_fn = nullptr;
  status = napi_create_function(env, "chdir", NAPI_AUTO_LENGTH, ProcessChdirCallback, nullptr, &chdir_fn);
  if (status != napi_ok || chdir_fn == nullptr) return (status == napi_ok) ? napi_generic_failure : status;
  status = napi_set_named_property(env, process_obj, "chdir", chdir_fn);
  if (status != napi_ok) return status;

  napi_value cpu_usage_fn = nullptr;
  status = napi_create_function(env, "cpuUsage", NAPI_AUTO_LENGTH, ProcessCpuUsageCallback, nullptr, &cpu_usage_fn);
  if (status != napi_ok || cpu_usage_fn == nullptr) return (status == napi_ok) ? napi_generic_failure : status;
  status = napi_set_named_property(env, process_obj, "cpuUsage", cpu_usage_fn);
  if (status != napi_ok) return status;

  napi_value available_memory_fn = nullptr;
  status = napi_create_function(env, "availableMemory", NAPI_AUTO_LENGTH, ProcessAvailableMemoryCallback, nullptr, &available_memory_fn);
  if (status != napi_ok || available_memory_fn == nullptr) return (status == napi_ok) ? napi_generic_failure : status;
  status = napi_set_named_property(env, process_obj, "availableMemory", available_memory_fn);
  if (status != napi_ok) return status;

  napi_value constrained_memory_fn = nullptr;
  status = napi_create_function(env, "constrainedMemory", NAPI_AUTO_LENGTH, ProcessConstrainedMemoryCallback, nullptr, &constrained_memory_fn);
  if (status != napi_ok || constrained_memory_fn == nullptr) return (status == napi_ok) ? napi_generic_failure : status;
  status = napi_set_named_property(env, process_obj, "constrainedMemory", constrained_memory_fn);
  if (status != napi_ok) return status;

  napi_value umask_fn = nullptr;
  status = napi_create_function(env, "umask", NAPI_AUTO_LENGTH, ProcessUmaskCallback, nullptr, &umask_fn);
  if (status != napi_ok || umask_fn == nullptr) return (status == napi_ok) ? napi_generic_failure : status;
  status = napi_set_named_property(env, process_obj, "umask", umask_fn);
  if (status != napi_ok) return status;

  napi_value uptime_fn = nullptr;
  status = napi_create_function(env, "uptime", NAPI_AUTO_LENGTH, ProcessUptimeCallback, nullptr, &uptime_fn);
  if (status != napi_ok || uptime_fn == nullptr) return (status == napi_ok) ? napi_generic_failure : status;
  status = napi_set_named_property(env, process_obj, "uptime", uptime_fn);
  if (status != napi_ok) return status;

  napi_value hrtime_fn = nullptr;
  status = napi_create_function(env, "hrtime", NAPI_AUTO_LENGTH, ProcessHrtimeCallback, nullptr, &hrtime_fn);
  if (status != napi_ok || hrtime_fn == nullptr) return (status == napi_ok) ? napi_generic_failure : status;
  napi_value hrtime_bigint_fn = nullptr;
  status = napi_create_function(env, "bigint", NAPI_AUTO_LENGTH, ProcessHrtimeBigintCallback, nullptr, &hrtime_bigint_fn);
  if (status != napi_ok || hrtime_bigint_fn == nullptr) return (status == napi_ok) ? napi_generic_failure : status;
  status = napi_set_named_property(env, hrtime_fn, "bigint", hrtime_bigint_fn);
  if (status != napi_ok) return status;
  status = napi_set_named_property(env, process_obj, "hrtime", hrtime_fn);
  if (status != napi_ok) return status;

  napi_value on_fn = nullptr;
  status = napi_create_function(env, "on", NAPI_AUTO_LENGTH, [](napi_env env, napi_callback_info info) {
    napi_value undefined = nullptr;
    napi_get_undefined(env, &undefined);
    return undefined;
  }, nullptr, &on_fn);
  if (status != napi_ok || on_fn == nullptr) return (status == napi_ok) ? napi_generic_failure : status;
  status = napi_set_named_property(env, process_obj, "on", on_fn);
  if (status != napi_ok) return status;
  status = napi_set_named_property(env, process_obj, "addListener", on_fn);
  if (status != napi_ok) return status;
  status = napi_set_named_property(env, process_obj, "once", on_fn);
  if (status != napi_ok) return status;

  napi_value remove_listener_fn = nullptr;
  status = napi_create_function(env, "removeListener", NAPI_AUTO_LENGTH, [](napi_env env, napi_callback_info info) {
    napi_value undefined = nullptr;
    napi_get_undefined(env, &undefined);
    return undefined;
  }, nullptr, &remove_listener_fn);
  if (status != napi_ok || remove_listener_fn == nullptr) return (status == napi_ok) ? napi_generic_failure : status;
  status = napi_set_named_property(env, process_obj, "removeListener", remove_listener_fn);
  if (status != napi_ok) return status;

  status = InstallProcessStream(env, process_obj, "stdout", 1, ProcessStdoutWriteCallback);
  if (status != napi_ok) return status;
  status = InstallProcessStream(env, process_obj, "stderr", 2, ProcessStderrWriteCallback);
  if (status != napi_ok) return status;

  napi_value abort_fn = nullptr;
  status = napi_create_function(env, "abort", NAPI_AUTO_LENGTH, ProcessAbortCallback, nullptr, &abort_fn);
  if (status != napi_ok || abort_fn == nullptr) return (status == napi_ok) ? napi_generic_failure : status;
  status = napi_set_named_property(env, process_obj, "abort", abort_fn);
  if (status != napi_ok) return status;

  napi_value exit_fn = nullptr;
  status = napi_create_function(env, "exit", NAPI_AUTO_LENGTH, ProcessExitCallback, nullptr, &exit_fn);
  if (status != napi_ok || exit_fn == nullptr) return (status == napi_ok) ? napi_generic_failure : status;
  status = napi_set_named_property(env, process_obj, "exit", exit_fn);
  if (status != napi_ok) return status;

  napi_value arch_str = nullptr;
  status = napi_create_string_utf8(env, DetectArch(), NAPI_AUTO_LENGTH, &arch_str);
  if (status != napi_ok || arch_str == nullptr) return (status == napi_ok) ? napi_generic_failure : status;
  status = napi_set_named_property(env, process_obj, "arch", arch_str);
  if (status != napi_ok) return status;

  napi_value platform_str = nullptr;
  status = napi_create_string_utf8(env, DetectPlatform(), NAPI_AUTO_LENGTH, &platform_str);
  if (status != napi_ok || platform_str == nullptr) return (status == napi_ok) ? napi_generic_failure : status;
  status = napi_set_named_property(env, process_obj, "platform", platform_str);
  if (status != napi_ok) return status;

  napi_value exec_path = nullptr;
  status = napi_create_string_utf8(env, g_unode_exec_path.c_str(), NAPI_AUTO_LENGTH, &exec_path);
  if (status != napi_ok || exec_path == nullptr) return (status == napi_ok) ? napi_generic_failure : status;
  status = napi_set_named_property(env, process_obj, "execPath", exec_path);
  if (status != napi_ok) return status;

  napi_value version_str = nullptr;
  status = napi_create_string_utf8(env, "v24.0.0", NAPI_AUTO_LENGTH, &version_str);
  if (status != napi_ok || version_str == nullptr) return (status == napi_ok) ? napi_generic_failure : status;
  status = napi_set_named_property(env, process_obj, "version", version_str);
  if (status != napi_ok) return status;

  napi_value pid_value = nullptr;
#if defined(_WIN32)
  status = napi_create_int32(env, 1, &pid_value);
#else
  status = napi_create_int32(env, static_cast<int32_t>(getpid()), &pid_value);
#endif
  if (status != napi_ok || pid_value == nullptr) return (status == napi_ok) ? napi_generic_failure : status;
  status = napi_set_named_property(env, process_obj, "pid", pid_value);
  if (status != napi_ok) return status;

  napi_value ppid_value = nullptr;
#if defined(_WIN32)
  status = napi_create_int32(env, 1, &ppid_value);
#else
  status = napi_create_int32(env, static_cast<int32_t>(getppid()), &ppid_value);
#endif
  if (status != napi_ok || ppid_value == nullptr) return (status == napi_ok) ? napi_generic_failure : status;
  status = napi_set_named_property(env, process_obj, "ppid", ppid_value);
  if (status != napi_ok) return status;

  napi_value versions_obj = nullptr;
  status = napi_create_object(env, &versions_obj);
  if (status != napi_ok || versions_obj == nullptr) return (status == napi_ok) ? napi_generic_failure : status;
  struct VersionEntry {
    const char* key;
    const char* value;
  };
  const VersionEntry version_entries[] = {
      {"node", "24.0.0"}, {"acorn", "8.15.0"}, {"ada", "2.9.2"}, {"ares", "1.34.5"},
      {"brotli", "1.1.0"}, {"cjs_module_lexer", "2.2.0"}, {"llhttp", "9.2.1"}, {"modules", "131"},
      {"napi", "8"}, {"nbytes", "0.1.1"}, {"ncrypto", "0.0.1"}, {"nghttp2", "1.61.0"}, {"openssl", "3.0.0"},
      {"simdjson", "3.13.0"},
      {"simdutf", "6.4.0"}, {"uv", "1.51.0"}, {"uvwasi", "0.0.21"}, {"v8", "14.5.201.9-node.0"},
      {"zlib", "1.3.1"}, {"zstd", "1.5.6"},
  };
  for (const auto& entry : version_entries) {
    napi_value value = nullptr;
    status = napi_create_string_utf8(env, entry.value, NAPI_AUTO_LENGTH, &value);
    if (status != napi_ok || value == nullptr) return (status == napi_ok) ? napi_generic_failure : status;
    napi_property_descriptor prop = {};
    prop.utf8name = entry.key;
    prop.value = value;
    prop.attributes = napi_enumerable;
    status = napi_define_properties(env, versions_obj, 1, &prop);
    if (status != napi_ok) return status;
  }
  status = napi_set_named_property(env, process_obj, "versions", versions_obj);
  if (status != napi_ok) return status;

  napi_value release_obj = nullptr;
  status = napi_create_object(env, &release_obj);
  if (status != napi_ok || release_obj == nullptr) return (status == napi_ok) ? napi_generic_failure : status;
  napi_value release_name = nullptr;
  status = napi_create_string_utf8(env, "node", NAPI_AUTO_LENGTH, &release_name);
  if (status != napi_ok || release_name == nullptr) return (status == napi_ok) ? napi_generic_failure : status;
  status = napi_set_named_property(env, release_obj, "name", release_name);
  if (status != napi_ok) return status;
  status = napi_set_named_property(env, process_obj, "release", release_obj);
  if (status != napi_ok) return status;

  napi_value features_obj = nullptr;
  status = napi_create_object(env, &features_obj);
  if (status != napi_ok || features_obj == nullptr) return (status == napi_ok) ? napi_generic_failure : status;
  napi_value inspector_true = nullptr;
  status = napi_get_boolean(env, true, &inspector_true);
  if (status != napi_ok || inspector_true == nullptr) return (status == napi_ok) ? napi_generic_failure : status;
  status = napi_set_named_property(env, features_obj, "inspector", inspector_true);
  if (status != napi_ok) return status;
  napi_value false_value = nullptr;
  status = napi_get_boolean(env, false, &false_value);
  if (status != napi_ok || false_value == nullptr) return (status == napi_ok) ? napi_generic_failure : status;
  const char* feature_keys[] = {"debug", "uv", "ipv6", "openssl_is_boringssl", "tls_alpn", "tls_sni",
                                "tls_ocsp", "tls", "cached_builtins", "require_module"};
  for (const char* key : feature_keys) {
    if (napi_set_named_property(env, features_obj, key, false_value) != napi_ok) return napi_generic_failure;
  }
  // Keep compatibility with currently imported crypto test subset.
  if (napi_set_named_property(env, features_obj, "openssl_is_boringssl", inspector_true) != napi_ok) {
    return napi_generic_failure;
  }
  if (napi_set_named_property(env, features_obj, "typescript", false_value) != napi_ok) return napi_generic_failure;
  status = napi_set_named_property(env, process_obj, "features", features_obj);
  if (status != napi_ok) return status;

  napi_value config_obj = nullptr;
  status = napi_create_object(env, &config_obj);
  if (status != napi_ok || config_obj == nullptr) return (status == napi_ok) ? napi_generic_failure : status;
  napi_value variables_obj = nullptr;
  status = napi_create_object(env, &variables_obj);
  if (status != napi_ok || variables_obj == nullptr) return (status == napi_ok) ? napi_generic_failure : status;
  napi_value zero = nullptr;
  status = napi_create_int32(env, 0, &zero);
  if (status != napi_ok || zero == nullptr) return (status == napi_ok) ? napi_generic_failure : status;
  const char* int_var_keys[] = {"v8_enable_i18n_support", "node_quic", "asan", "node_shared_openssl"};
  for (const char* key : int_var_keys) {
    if (napi_set_named_property(env, variables_obj, key, zero) != napi_ok) return napi_generic_failure;
  }
  napi_value empty_shareable_builtins = nullptr;
  status = napi_create_array_with_length(env, 0, &empty_shareable_builtins);
  if (status != napi_ok || empty_shareable_builtins == nullptr) return (status == napi_ok) ? napi_generic_failure : status;
  status = napi_set_named_property(env, variables_obj, "node_builtin_shareable_builtins", empty_shareable_builtins);
  if (status != napi_ok) return status;
  napi_value napi_build_version = nullptr;
  status = napi_create_string_utf8(env, "8", NAPI_AUTO_LENGTH, &napi_build_version);
  if (status != napi_ok || napi_build_version == nullptr) return (status == napi_ok) ? napi_generic_failure : status;
  status = napi_set_named_property(env, variables_obj, "napi_build_version", napi_build_version);
  if (status != napi_ok) return status;
  napi_property_descriptor variables_desc = {};
  variables_desc.utf8name = "variables";
  variables_desc.value = variables_obj;
  variables_desc.attributes = napi_enumerable;
  status = napi_define_properties(env, config_obj, 1, &variables_desc);
  if (status != napi_ok) return status;
  napi_property_descriptor config_desc = {};
  config_desc.utf8name = "config";
  config_desc.value = config_obj;
  config_desc.attributes = napi_enumerable;
  status = napi_define_properties(env, process_obj, 1, &config_desc);
  if (status != napi_ok) return status;

  return napi_set_named_property(env, global, "process", process_obj);
}
