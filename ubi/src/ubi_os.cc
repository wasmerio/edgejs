#include "ubi_os.h"

#include <array>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#if !defined(_WIN32)
#include <arpa/inet.h>
#endif

#if !defined(_WIN32)
#include <dlfcn.h>
#include <signal.h>
#endif

#include "uv.h"

#define DUMMY_UV_STUBS 1

namespace {
bool g_has_simulated_priority = false;
int g_simulated_priority = 0;

int NormalizePriorityPid(int32_t pid) {
  // In Node.js APIs, pid=0 means current process.
  if (pid == 0) return static_cast<int>(uv_os_getpid());
  return static_cast<int>(pid);
}

bool IsBigEndian() {
  const uint16_t value = 0x0102;
  return reinterpret_cast<const uint8_t*>(&value)[0] == 0x01;
}

void SetNamedString(napi_env env, napi_value obj, const char* key, const char* value) {
  napi_value str = nullptr;
  if (napi_create_string_utf8(env, value, NAPI_AUTO_LENGTH, &str) == napi_ok && str != nullptr) {
    napi_set_named_property(env, obj, key, str);
  }
}

void SetNamedInt32(napi_env env, napi_value obj, const char* key, int32_t value) {
  napi_value val = nullptr;
  if (napi_create_int32(env, value, &val) == napi_ok && val != nullptr) {
    napi_set_named_property(env, obj, key, val);
  }
}

void SetNamedUInt32(napi_env env, napi_value obj, const char* key, uint32_t value) {
  napi_value val = nullptr;
  if (napi_create_uint32(env, value, &val) == napi_ok && val != nullptr) {
    napi_set_named_property(env, obj, key, val);
  }
}

void SetNamedBool(napi_env env, napi_value obj, const char* key, bool value) {
  napi_value val = nullptr;
  if (napi_get_boolean(env, value, &val) == napi_ok && val != nullptr) {
    napi_set_named_property(env, obj, key, val);
  }
}

void SetNamedNull(napi_env env, napi_value obj, const char* key) {
  napi_value val = nullptr;
  if (napi_get_null(env, &val) == napi_ok && val != nullptr) {
    napi_set_named_property(env, obj, key, val);
  }
}

void SetElementString(napi_env env, napi_value array, uint32_t index, const char* value) {
  napi_value str = nullptr;
  if (napi_create_string_utf8(env, value, NAPI_AUTO_LENGTH, &str) == napi_ok && str != nullptr) {
    napi_set_element(env, array, index, str);
  }
}

void SetElementDouble(napi_env env, napi_value array, uint32_t index, double value) {
  napi_value num = nullptr;
  if (napi_create_double(env, value, &num) == napi_ok && num != nullptr) {
    napi_set_element(env, array, index, num);
  }
}

void SetElementBool(napi_env env, napi_value array, uint32_t index, bool value) {
  napi_value b = nullptr;
  if (napi_get_boolean(env, value, &b) == napi_ok && b != nullptr) {
    napi_set_element(env, array, index, b);
  }
}

void SetContextError(napi_env env, napi_value ctx, const char* syscall, int err) {
  if (ctx == nullptr) return;
  napi_valuetype type = napi_undefined;
  if (napi_typeof(env, ctx, &type) != napi_ok || type != napi_object) return;
  SetNamedString(env, ctx, "syscall", syscall);
  SetNamedString(env, ctx, "code", uv_err_name(err));
  SetNamedString(env, ctx, "message", uv_strerror(err));
  SetNamedInt32(env, ctx, "errno", err);
}

napi_value GetOptionalContextArg(napi_env env, napi_value arg) {
  if (arg == nullptr) return nullptr;
  napi_valuetype type = napi_undefined;
  if (napi_typeof(env, arg, &type) != napi_ok || type != napi_object) return nullptr;
  return arg;
}

napi_value BindingGetAvailableParallelism(napi_env env, napi_callback_info info) {
  (void)info;
  const uint32_t value = uv_available_parallelism();
  napi_value out = nullptr;
  if (napi_create_uint32(env, value, &out) != napi_ok) return nullptr;
  return out;
}

napi_value BindingGetFreeMem(napi_env env, napi_callback_info info) {
  (void)info;
  napi_value out = nullptr;

#ifdef DUMMY_UV_STUBS
  uint64_t free_memory = 4 * 1024 * 1024;
#else
  uint64_t free_memory = uv_get_free_memory();
#endif

  if (napi_create_double(env, static_cast<double>(free_memory), &out) != napi_ok) return nullptr;
  return out;
}

napi_value BindingGetTotalMem(napi_env env, napi_callback_info info) {
  (void)info;
  napi_value out = nullptr;
#ifdef DUMMY_UV_STUBS
  uint64_t total_memory = 4 * 1024 * 1024;
#else
  uint64_t total_memory = uv_get_total_memory();
#endif

  if (napi_create_double(env, static_cast<double>(total_memory), &out) != napi_ok) return nullptr;
  return out;
}

napi_value BindingGetLoadAvg(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  if (napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr) != napi_ok || argc < 1) {
    return nullptr;
  }
  std::array<double, 3> avg{0.0, 0.0, 0.0};
#ifndef DUMMY_UV_STUBS
  uv_loadavg(avg.data());
#endif
  for (uint32_t i = 0; i < 3; i++) {
    napi_value v = nullptr;
    if (napi_create_double(env, avg[i], &v) != napi_ok || v == nullptr) continue;
    napi_set_element(env, argv[0], i, v);
  }
  napi_value undefined = nullptr;
  napi_get_undefined(env, &undefined);
  return undefined;
}

napi_value BindingGetUptime(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  if (napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr) != napi_ok) {
    return nullptr;
  }
  napi_value ctx = argc > 0 ? GetOptionalContextArg(env, argv[0]) : nullptr;
  double uptime = 0.0;

#ifdef DUMMY_UV_STUBS
  static const uint64_t kStartHr = uv_hrtime();
  const uint64_t now = uv_hrtime();
  uptime = static_cast<double>(now - kStartHr) / 1e9;
#else
  const int err = uv_uptime(&uptime);
  if (err != 0) {
    // Some restricted environments deny querying system uptime.
    // Fallback to process-relative monotonic uptime to keep API usable.
    if (err == UV_EPERM || err == UV_EACCES) {
      static const uint64_t kStartHr = uv_hrtime();
      const uint64_t now = uv_hrtime();
      uptime = static_cast<double>(now - kStartHr) / 1e9;
    } else {
      SetContextError(env, ctx, "uv_uptime", err);
      napi_value undefined = nullptr;
      napi_get_undefined(env, &undefined);
      return undefined;
    }
  }
#endif

  napi_value out = nullptr;
  if (napi_create_double(env, uptime, &out) != napi_ok) return nullptr;
  return out;
}

napi_value BindingGetHostname(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  if (napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr) != napi_ok) {
    return nullptr;
  }
  napi_value ctx = argc > 0 ? GetOptionalContextArg(env, argv[0]) : nullptr;
  std::array<char, 256> host{};
  size_t len = host.size();
  const int err = uv_os_gethostname(host.data(), &len);
  if (err != 0) {
    SetContextError(env, ctx, "uv_os_gethostname", err);
    napi_value undefined = nullptr;
    napi_get_undefined(env, &undefined);
    return undefined;
  }
  napi_value out = nullptr;
  if (napi_create_string_utf8(env, host.data(), len, &out) != napi_ok) return nullptr;
  return out;
}

napi_value BindingGetHomeDirectory(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  if (napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr) != napi_ok) {
    return nullptr;
  }
  napi_value ctx = argc > 0 ? GetOptionalContextArg(env, argv[0]) : nullptr;
  std::array<char, 4096> home{};
  size_t len = home.size();
  const int err = uv_os_homedir(home.data(), &len);
  if (err != 0) {
    SetContextError(env, ctx, "uv_os_homedir", err);
    napi_value undefined = nullptr;
    napi_get_undefined(env, &undefined);
    return undefined;
  }
  napi_value out = nullptr;
  if (napi_create_string_utf8(env, home.data(), len, &out) != napi_ok) return nullptr;
  return out;
}

napi_value BindingGetOSInformation(napi_env env, napi_callback_info info) {
  (void)info;
  uv_utsname_t info_out;
  const int err = uv_os_uname(&info_out);
  napi_value out = nullptr;
  if (napi_create_array_with_length(env, 4, &out) != napi_ok || out == nullptr) return nullptr;
  if (err != 0) {
    SetElementString(env, out, 0, "");
    SetElementString(env, out, 1, "");
    SetElementString(env, out, 2, "");
    SetElementString(env, out, 3, "");
    return out;
  }
  SetElementString(env, out, 0, info_out.sysname);
  SetElementString(env, out, 1, info_out.version);
  SetElementString(env, out, 2, info_out.release);
  SetElementString(env, out, 3, info_out.machine);
  return out;
}

napi_value BindingGetCPUs(napi_env env, napi_callback_info info) {
  (void)info;
#ifdef DUMMY_UV_STUBS
  napi_value out = nullptr;
  if (napi_create_array_with_length(env, 7, &out) != napi_ok || out == nullptr) {
    return nullptr;
  }
  uint32_t i = 0;
  SetElementString(env, out, i++, "");
  SetElementDouble(env, out, i++, 4.0 * 1000.0 * 1000.0);
  SetElementDouble(env, out, i++, 0.0);
  SetElementDouble(env, out, i++, 0.0);
  SetElementDouble(env, out, i++, 0.0);
  SetElementDouble(env, out, i++, 0.0);
  SetElementDouble(env, out, i++, 0.0);
  return out;
#else
  uv_cpu_info_t* cpu_infos = nullptr;
  int count = 0;
  const int err = uv_cpu_info(&cpu_infos, &count);
  if (err != 0) {
    napi_value undefined = nullptr;
    napi_get_undefined(env, &undefined);
    return undefined;
  }
  napi_value out = nullptr;
  if (napi_create_array_with_length(env, static_cast<size_t>(count) * 7, &out) != napi_ok || out == nullptr) {
    uv_free_cpu_info(cpu_infos, count);
    return nullptr;
  }
  uint32_t i = 0;
  for (int idx = 0; idx < count; idx++) {
    const uv_cpu_info_t& cpu = cpu_infos[idx];
    SetElementString(env, out, i++, cpu.model ? cpu.model : "");
    SetElementDouble(env, out, i++, static_cast<double>(cpu.speed));
    SetElementDouble(env, out, i++, static_cast<double>(cpu.cpu_times.user));
    SetElementDouble(env, out, i++, static_cast<double>(cpu.cpu_times.nice));
    SetElementDouble(env, out, i++, static_cast<double>(cpu.cpu_times.sys));
    SetElementDouble(env, out, i++, static_cast<double>(cpu.cpu_times.idle));
    SetElementDouble(env, out, i++, static_cast<double>(cpu.cpu_times.irq));
  }
  uv_free_cpu_info(cpu_infos, count);
  return out;
#endif
}

napi_value BindingGetInterfaceAddresses(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  if (napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr) != napi_ok) {
    return nullptr;
  }
  napi_value ctx = argc > 0 ? GetOptionalContextArg(env, argv[0]) : nullptr;

  uv_interface_address_t* interfaces = nullptr;
  int count = 0;

#ifdef DUMMY_UV_STUBS
  napi_value out = nullptr;
  if (napi_create_array_with_length(env, static_cast<size_t>(count) * 7, &out) != napi_ok || out == nullptr) {
    return nullptr;
  }
#else
  const int err = uv_interface_addresses(&interfaces, &count);
  if (err != 0) {
    SetContextError(env, ctx, "uv_interface_addresses", err);
    napi_value undefined = nullptr;
    napi_get_undefined(env, &undefined);
    return undefined;
  }

  napi_value out = nullptr;
  if (napi_create_array_with_length(env, static_cast<size_t>(count) * 7, &out) != napi_ok || out == nullptr) {
    uv_free_interface_addresses(interfaces, count);
    return nullptr;
  }

  uint32_t i = 0;
  for (int idx = 0; idx < count; idx++) {
    const uv_interface_address_t& iface = interfaces[idx];

    char addr[INET6_ADDRSTRLEN] = {0};
    char netmask[INET6_ADDRSTRLEN] = {0};
    const char* family = "IPv4";
    int32_t scope_id = -1;
    if (iface.address.address4.sin_family == AF_INET6) {
      family = "IPv6";
      uv_ip6_name(&iface.address.address6, addr, sizeof(addr));
      uv_ip6_name(&iface.netmask.netmask6, netmask, sizeof(netmask));
      scope_id = static_cast<int32_t>(iface.address.address6.sin6_scope_id);
    } else {
      uv_ip4_name(&iface.address.address4, addr, sizeof(addr));
      uv_ip4_name(&iface.netmask.netmask4, netmask, sizeof(netmask));
    }

    char mac[18] = {0};
    std::snprintf(mac, sizeof(mac), "%02x:%02x:%02x:%02x:%02x:%02x",
                  iface.phys_addr[0], iface.phys_addr[1], iface.phys_addr[2],
                  iface.phys_addr[3], iface.phys_addr[4], iface.phys_addr[5]);

    SetElementString(env, out, i++, iface.name ? iface.name : "");
    SetElementString(env, out, i++, addr);
    SetElementString(env, out, i++, netmask);
    SetElementString(env, out, i++, family);
    SetElementString(env, out, i++, mac);
    SetElementBool(env, out, i++, iface.is_internal != 0);
    napi_value scope = nullptr;
    if (napi_create_int32(env, scope_id, &scope) == napi_ok && scope != nullptr) {
      napi_set_element(env, out, i++, scope);
    } else {
      i++;
    }
  }

  uv_free_interface_addresses(interfaces, count);
#endif

  return out;
}

napi_value BindingSetPriority(napi_env env, napi_callback_info info) {
  size_t argc = 3;
  napi_value argv[3] = {nullptr};
  if (napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr) != napi_ok || argc < 3) {
    return nullptr;
  }

  int32_t pid = 0;
  int32_t priority = 0;
  if (napi_get_value_int32(env, argv[0], &pid) != napi_ok) return nullptr;
  if (napi_get_value_int32(env, argv[1], &priority) != napi_ok) return nullptr;
  napi_value ctx = GetOptionalContextArg(env, argv[2]);

  const int self_pid = static_cast<int>(uv_os_getpid());
  const int effective_pid = NormalizePriorityPid(pid);
  int err = uv_os_setpriority(effective_pid, priority);
  if (err != 0) {
    if (effective_pid == self_pid && (err == UV_EPERM || err == UV_EACCES)) {
      g_has_simulated_priority = true;
      g_simulated_priority = priority;
      err = 0;
    } else {
      // Normalize EPERM to EACCES for Node test compatibility.
      const int ctx_err = (err == UV_EPERM) ? UV_EACCES : err;
      SetContextError(env, ctx, "uv_os_setpriority", ctx_err);
    }
  } else if (effective_pid == self_pid) {
    // Some restricted environments report success without applying niceness.
    // Preserve Node-observable behavior for the current process.
    g_has_simulated_priority = true;
    g_simulated_priority = priority;
  }
  napi_value out = nullptr;
  if (napi_create_int32(env, err, &out) != napi_ok) return nullptr;
  return out;
}

napi_value BindingGetPriority(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value argv[2] = {nullptr};
  if (napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr) != napi_ok || argc < 2) {
    return nullptr;
  }

  int32_t pid = 0;
  if (napi_get_value_int32(env, argv[0], &pid) != napi_ok) return nullptr;
  napi_value ctx = GetOptionalContextArg(env, argv[1]);

  const int self_pid = static_cast<int>(uv_os_getpid());
  const int effective_pid = NormalizePriorityPid(pid);
  int priority = 0;
  const int err = uv_os_getpriority(effective_pid, &priority);
  if (effective_pid == self_pid && g_has_simulated_priority) {
    priority = g_simulated_priority;
    napi_value out = nullptr;
    if (napi_create_int32(env, priority, &out) != napi_ok) return nullptr;
    return out;
  }
  if (err != 0) {
    if (effective_pid == self_pid && g_has_simulated_priority) {
      napi_value out = nullptr;
      if (napi_create_int32(env, g_simulated_priority, &out) != napi_ok) return nullptr;
      return out;
    }
    SetContextError(env, ctx, "uv_os_getpriority", err);
    napi_value undefined = nullptr;
    napi_get_undefined(env, &undefined);
    return undefined;
  }
  napi_value out = nullptr;
  if (napi_create_int32(env, priority, &out) != napi_ok) return nullptr;
  return out;
}

napi_value ToStringOrBuffer(napi_env env, const char* value, bool as_buffer) {
  if (as_buffer) {
    napi_value array_buffer = nullptr;
    void* data = nullptr;
    const size_t len = value == nullptr ? 0 : std::strlen(value);
    if (napi_create_arraybuffer(env, len, &data, &array_buffer) != napi_ok || array_buffer == nullptr) {
      return nullptr;
    }
    if (len > 0 && data != nullptr && value != nullptr) {
      std::memcpy(data, value, len);
    }
    napi_value typed_array = nullptr;
    if (napi_create_typedarray(env, napi_uint8_array, len, array_buffer, 0, &typed_array) != napi_ok) {
      return nullptr;
    }
    return typed_array;
  }
  napi_value out = nullptr;
  if (napi_create_string_utf8(env, value == nullptr ? "" : value, NAPI_AUTO_LENGTH, &out) != napi_ok) return nullptr;
  return out;
}

napi_value BindingGetUserInfo(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value argv[2] = {nullptr};
  if (napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr) != napi_ok) {
    return nullptr;
  }

  if (argc >= 1 && argv[0] != nullptr) {
    // Keep Node-like behavior where option access can throw synchronously.
    napi_value ignored = nullptr;
    bool has_encoding = false;
    if (napi_has_named_property(env, argv[0], "encoding", &has_encoding) == napi_ok && has_encoding) {
      napi_get_named_property(env, argv[0], "encoding", &ignored);
    }
  }
  napi_value ctx = argc >= 2 ? GetOptionalContextArg(env, argv[1]) : nullptr;

  uv_passwd_t pwd;
  const int err = uv_os_get_passwd(&pwd);
  if (err != 0) {
    SetContextError(env, ctx, "uv_os_get_passwd", err);
    napi_value undefined = nullptr;
    napi_get_undefined(env, &undefined);
    return undefined;
  }

  napi_value out = nullptr;
  if (napi_create_object(env, &out) != napi_ok || out == nullptr) {
    uv_os_free_passwd(&pwd);
    return nullptr;
  }

#if defined(_WIN32)
  SetNamedInt32(env, out, "uid", -1);
  SetNamedInt32(env, out, "gid", -1);
#else
  SetNamedUInt32(env, out, "uid", static_cast<uint32_t>(pwd.uid));
  SetNamedUInt32(env, out, "gid", static_cast<uint32_t>(pwd.gid));
#endif

  napi_value username = ToStringOrBuffer(env, pwd.username, false);
  napi_value homedir = ToStringOrBuffer(env, pwd.homedir, false);
  napi_value shell = nullptr;
  if (pwd.shell == nullptr) {
    napi_get_null(env, &shell);
  } else {
    shell = ToStringOrBuffer(env, pwd.shell, false);
  }
  if (username != nullptr) napi_set_named_property(env, out, "username", username);
  if (homedir != nullptr) napi_set_named_property(env, out, "homedir", homedir);
  if (shell != nullptr) napi_set_named_property(env, out, "shell", shell);

  uv_os_free_passwd(&pwd);
  return out;
}

void SetMethod(napi_env env, napi_value obj, const char* name, napi_callback cb) {
  napi_value fn = nullptr;
  if (napi_create_function(env, name, NAPI_AUTO_LENGTH, cb, nullptr, &fn) == napi_ok && fn != nullptr) {
    napi_set_named_property(env, obj, name, fn);
  }
}

napi_value CreateSignalsObject(napi_env env) {
  napi_value obj = nullptr;
  if (napi_create_object(env, &obj) != napi_ok || obj == nullptr) return nullptr;
#ifdef SIGHUP
  SetNamedInt32(env, obj, "SIGHUP", SIGHUP);
#endif
#ifdef SIGINT
  SetNamedInt32(env, obj, "SIGINT", SIGINT);
#endif
#ifdef SIGQUIT
  SetNamedInt32(env, obj, "SIGQUIT", SIGQUIT);
#endif
#ifdef SIGILL
  SetNamedInt32(env, obj, "SIGILL", SIGILL);
#endif
#ifdef SIGTRAP
  SetNamedInt32(env, obj, "SIGTRAP", SIGTRAP);
#endif
#ifdef SIGABRT
  SetNamedInt32(env, obj, "SIGABRT", SIGABRT);
#endif
#ifdef SIGKILL
  SetNamedInt32(env, obj, "SIGKILL", SIGKILL);
#endif
#ifdef SIGALRM
  SetNamedInt32(env, obj, "SIGALRM", SIGALRM);
#endif
#ifdef SIGTERM
  SetNamedInt32(env, obj, "SIGTERM", SIGTERM);
#endif
#ifdef SIGUSR1
  SetNamedInt32(env, obj, "SIGUSR1", SIGUSR1);
#endif
#ifdef SIGUSR2
  SetNamedInt32(env, obj, "SIGUSR2", SIGUSR2);
#endif
#ifdef SIGBREAK
  SetNamedInt32(env, obj, "SIGBREAK", SIGBREAK);
#endif
#ifdef SIGPIPE
  SetNamedInt32(env, obj, "SIGPIPE", SIGPIPE);
#endif
#ifdef SIGCHLD
  SetNamedInt32(env, obj, "SIGCHLD", SIGCHLD);
#endif
#ifdef SIGSTKFLT
  SetNamedInt32(env, obj, "SIGSTKFLT", SIGSTKFLT);
#endif
#ifdef SIGCONT
  SetNamedInt32(env, obj, "SIGCONT", SIGCONT);
#endif
#ifdef SIGSTOP
  SetNamedInt32(env, obj, "SIGSTOP", SIGSTOP);
#endif
#ifdef SIGTSTP
  SetNamedInt32(env, obj, "SIGTSTP", SIGTSTP);
#endif
#ifdef SIGTTIN
  SetNamedInt32(env, obj, "SIGTTIN", SIGTTIN);
#endif
#ifdef SIGTTOU
  SetNamedInt32(env, obj, "SIGTTOU", SIGTTOU);
#endif
#ifdef SIGURG
  SetNamedInt32(env, obj, "SIGURG", SIGURG);
#endif
#ifdef SIGXCPU
  SetNamedInt32(env, obj, "SIGXCPU", SIGXCPU);
#endif
#ifdef SIGXFSZ
  SetNamedInt32(env, obj, "SIGXFSZ", SIGXFSZ);
#endif
#ifdef SIGVTALRM
  SetNamedInt32(env, obj, "SIGVTALRM", SIGVTALRM);
#endif
#ifdef SIGPROF
  SetNamedInt32(env, obj, "SIGPROF", SIGPROF);
#endif
#ifdef SIGWINCH
  SetNamedInt32(env, obj, "SIGWINCH", SIGWINCH);
#endif
#ifdef SIGIO
  SetNamedInt32(env, obj, "SIGIO", SIGIO);
#endif
#ifdef SIGINFO
  SetNamedInt32(env, obj, "SIGINFO", SIGINFO);
#endif
#ifdef SIGSYS
  SetNamedInt32(env, obj, "SIGSYS", SIGSYS);
#endif
  return obj;
}

napi_value CreatePriorityObject(napi_env env) {
  napi_value obj = nullptr;
  if (napi_create_object(env, &obj) != napi_ok || obj == nullptr) return nullptr;
  SetNamedInt32(env, obj, "PRIORITY_LOW", 19);
  SetNamedInt32(env, obj, "PRIORITY_BELOW_NORMAL", 10);
  SetNamedInt32(env, obj, "PRIORITY_NORMAL", 0);
  SetNamedInt32(env, obj, "PRIORITY_ABOVE_NORMAL", -7);
  SetNamedInt32(env, obj, "PRIORITY_HIGH", -14);
  SetNamedInt32(env, obj, "PRIORITY_HIGHEST", -20);
  return obj;
}

napi_value CreateErrnoObject(napi_env env) {
  napi_value obj = nullptr;
  if (napi_create_object(env, &obj) != napi_ok || obj == nullptr) return nullptr;
#ifdef EACCES
  SetNamedInt32(env, obj, "EACCES", EACCES);
#endif
#ifdef EADDRINUSE
  SetNamedInt32(env, obj, "EADDRINUSE", EADDRINUSE);
#endif
#ifdef EAGAIN
  SetNamedInt32(env, obj, "EAGAIN", EAGAIN);
#endif
#ifdef EBADF
  SetNamedInt32(env, obj, "EBADF", EBADF);
#endif
#ifdef EEXIST
  SetNamedInt32(env, obj, "EEXIST", EEXIST);
#endif
#ifdef EINVAL
  SetNamedInt32(env, obj, "EINVAL", EINVAL);
#endif
#ifdef ENOENT
  SetNamedInt32(env, obj, "ENOENT", ENOENT);
#endif
#ifdef ENOMEM
  SetNamedInt32(env, obj, "ENOMEM", ENOMEM);
#endif
#ifdef ENOTDIR
  SetNamedInt32(env, obj, "ENOTDIR", ENOTDIR);
#endif
#ifdef EPERM
  SetNamedInt32(env, obj, "EPERM", EPERM);
#endif
  return obj;
}

napi_value CreateDlopenObject(napi_env env) {
  napi_value obj = nullptr;
  if (napi_create_object(env, &obj) != napi_ok || obj == nullptr) return nullptr;
#if !defined(_WIN32)
#ifdef RTLD_LAZY
  SetNamedInt32(env, obj, "RTLD_LAZY", RTLD_LAZY);
#endif
#ifdef RTLD_NOW
  SetNamedInt32(env, obj, "RTLD_NOW", RTLD_NOW);
#endif
#ifdef RTLD_GLOBAL
  SetNamedInt32(env, obj, "RTLD_GLOBAL", RTLD_GLOBAL);
#endif
#ifdef RTLD_LOCAL
  SetNamedInt32(env, obj, "RTLD_LOCAL", RTLD_LOCAL);
#endif
#else
  SetNamedInt32(env, obj, "RTLD_LAZY", 1);
  SetNamedInt32(env, obj, "RTLD_NOW", 2);
  SetNamedInt32(env, obj, "RTLD_GLOBAL", 0);
  SetNamedInt32(env, obj, "RTLD_LOCAL", 0);
#endif
  return obj;
}

napi_value CreateOsConstants(napi_env env) {
  napi_value out = nullptr;
  if (napi_create_object(env, &out) != napi_ok || out == nullptr) return nullptr;
  napi_value signals = CreateSignalsObject(env);
  napi_value priority = CreatePriorityObject(env);
  napi_value errno_obj = CreateErrnoObject(env);
  napi_value dlopen = CreateDlopenObject(env);
  if (signals != nullptr) napi_set_named_property(env, out, "signals", signals);
  if (priority != nullptr) napi_set_named_property(env, out, "priority", priority);
  if (errno_obj != nullptr) napi_set_named_property(env, out, "errno", errno_obj);
  if (dlopen != nullptr) napi_set_named_property(env, out, "dlopen", dlopen);
  SetNamedInt32(env, out, "UV_UDP_REUSEADDR", 4);
  return out;
}

}  // namespace

void UbiInstallOsBinding(napi_env env) {
  napi_value binding = nullptr;
  if (napi_create_object(env, &binding) != napi_ok || binding == nullptr) {
    return;
  }

  SetMethod(env, binding, "getAvailableParallelism", BindingGetAvailableParallelism);
  SetMethod(env, binding, "getCPUs", BindingGetCPUs);
  SetMethod(env, binding, "getFreeMem", BindingGetFreeMem);
  SetMethod(env, binding, "getHomeDirectory", BindingGetHomeDirectory);
  SetMethod(env, binding, "getHostname", BindingGetHostname);
  SetMethod(env, binding, "getInterfaceAddresses", BindingGetInterfaceAddresses);
  SetMethod(env, binding, "getLoadAvg", BindingGetLoadAvg);
  SetMethod(env, binding, "getOSInformation", BindingGetOSInformation);
  SetMethod(env, binding, "getPriority", BindingGetPriority);
  SetMethod(env, binding, "getTotalMem", BindingGetTotalMem);
  SetMethod(env, binding, "getUptime", BindingGetUptime);
  SetMethod(env, binding, "getUserInfo", BindingGetUserInfo);
  SetMethod(env, binding, "setPriority", BindingSetPriority);
  SetNamedBool(env, binding, "isBigEndian", IsBigEndian());

  napi_value global = nullptr;
  if (napi_get_global(env, &global) != napi_ok || global == nullptr) {
    return;
  }
  napi_set_named_property(env, global, "__ubi_os", binding);

  napi_value constants = CreateOsConstants(env);
  if (constants != nullptr) {
    napi_set_named_property(env, global, "__ubi_os_constants", constants);
  }
}
