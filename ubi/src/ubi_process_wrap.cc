#include "ubi_process_wrap.h"

#include <cstdint>
#include <csignal>
#include <string>
#include <vector>

#include <uv.h>

#include "ubi_pipe_wrap.h"
#include "ubi_runtime.h"

namespace {

struct ProcessWrap {
  napi_env env = nullptr;
  napi_ref wrapper_ref = nullptr;
  bool wrapper_ref_held = false;
  int32_t pid = 0;
  bool alive = false;
  bool real_async = false;
  bool process_initialized = false;
  bool process_closed = false;
  bool delete_on_close = false;
  uv_process_t process{};
};

int32_t g_next_pid = 40000;

void HoldWrapperRef(ProcessWrap* wrap) {
  if (wrap == nullptr || wrap->env == nullptr || wrap->wrapper_ref == nullptr || wrap->wrapper_ref_held) return;
  uint32_t out = 0;
  if (napi_reference_ref(wrap->env, wrap->wrapper_ref, &out) == napi_ok) {
    wrap->wrapper_ref_held = true;
  }
}

void ReleaseWrapperRef(ProcessWrap* wrap) {
  if (wrap == nullptr || wrap->env == nullptr || wrap->wrapper_ref == nullptr || !wrap->wrapper_ref_held) return;
  uint32_t out = 0;
  if (napi_reference_unref(wrap->env, wrap->wrapper_ref, &out) == napi_ok) {
    wrap->wrapper_ref_held = false;
  }
}

bool IsTruthyProperty(napi_env env, napi_value obj, const char* key) {
  bool has = false;
  if (napi_has_named_property(env, obj, key, &has) != napi_ok || !has) return false;
  napi_value value = nullptr;
  if (napi_get_named_property(env, obj, key, &value) != napi_ok || value == nullptr) return false;
  bool out = false;
  if (napi_get_value_bool(env, value, &out) == napi_ok) return out;
  return false;
}

bool GetInt32Property(napi_env env, napi_value obj, const char* key, int32_t* out) {
  bool has = false;
  if (napi_has_named_property(env, obj, key, &has) != napi_ok || !has) return false;
  napi_value value = nullptr;
  if (napi_get_named_property(env, obj, key, &value) != napi_ok || value == nullptr) return false;
  return napi_get_value_int32(env, value, out) == napi_ok;
}

bool GetStringProperty(napi_env env, napi_value obj, const char* key, std::string* out) {
  bool has = false;
  if (napi_has_named_property(env, obj, key, &has) != napi_ok || !has) return false;
  napi_value value = nullptr;
  if (napi_get_named_property(env, obj, key, &value) != napi_ok || value == nullptr) return false;
  size_t len = 0;
  if (napi_get_value_string_utf8(env, value, nullptr, 0, &len) != napi_ok) return false;
  std::string s(len + 1, '\0');
  size_t copied = 0;
  if (napi_get_value_string_utf8(env, value, s.data(), s.size(), &copied) != napi_ok) return false;
  s.resize(copied);
  *out = std::move(s);
  return true;
}

bool GetNamedValue(napi_env env, napi_value obj, const char* key, napi_value* out) {
  if (out == nullptr) return false;
  bool has = false;
  if (napi_has_named_property(env, obj, key, &has) != napi_ok || !has) return false;
  return napi_get_named_property(env, obj, key, out) == napi_ok && *out != nullptr;
}

int SignalFromName(const std::string& name) {
  if (name == "SIGTERM") return SIGTERM;
  if (name == "SIGKILL") return SIGKILL;
  if (name == "SIGINT") return SIGINT;
  if (name == "SIGHUP") return SIGHUP;
  if (name == "SIGQUIT") return SIGQUIT;
  if (name == "SIGABRT") return SIGABRT;
  if (name == "SIGALRM") return SIGALRM;
  if (name == "SIGUSR1") return SIGUSR1;
  if (name == "SIGUSR2") return SIGUSR2;
  return 0;
}

const char* SignalNameFromNumber(int sig) {
  switch (sig) {
    case SIGTERM:
      return "SIGTERM";
    case SIGKILL:
      return "SIGKILL";
    case SIGINT:
      return "SIGINT";
    case SIGHUP:
      return "SIGHUP";
    case SIGQUIT:
      return "SIGQUIT";
    case SIGABRT:
      return "SIGABRT";
    case SIGALRM:
      return "SIGALRM";
    case SIGUSR1:
      return "SIGUSR1";
    case SIGUSR2:
      return "SIGUSR2";
    default:
      return nullptr;
  }
}

ProcessWrap* UnwrapProcess(napi_env env, napi_callback_info info, napi_value* self_out) {
  size_t argc = 0;
  napi_value self = nullptr;
  if (napi_get_cb_info(env, info, &argc, nullptr, &self, nullptr) != napi_ok || self == nullptr) return nullptr;
  ProcessWrap* wrap = nullptr;
  if (napi_unwrap(env, self, reinterpret_cast<void**>(&wrap)) != napi_ok || wrap == nullptr) return nullptr;
  if (self_out != nullptr) *self_out = self;
  return wrap;
}

napi_value MakeInt32(napi_env env, int32_t value) {
  napi_value out = nullptr;
  napi_create_int32(env, value, &out);
  return out;
}

void SetPidProperty(napi_env env, napi_value self, int32_t pid) {
  napi_value pid_value = MakeInt32(env, pid);
  if (pid_value != nullptr) napi_set_named_property(env, self, "pid", pid_value);
}

void EmitOnExit(ProcessWrap* wrap, int32_t exit_code, int32_t signal_code, bool has_signal) {
  if (wrap == nullptr || wrap->env == nullptr || wrap->wrapper_ref == nullptr) return;
  napi_value self = nullptr;
  if (napi_get_reference_value(wrap->env, wrap->wrapper_ref, &self) != napi_ok || self == nullptr) return;
  napi_value onexit = nullptr;
  bool has_onexit = false;
  if (napi_has_named_property(wrap->env, self, "onexit", &has_onexit) != napi_ok || !has_onexit) return;
  if (napi_get_named_property(wrap->env, self, "onexit", &onexit) != napi_ok || onexit == nullptr) return;
  napi_valuetype fn_type = napi_undefined;
  if (napi_typeof(wrap->env, onexit, &fn_type) != napi_ok || fn_type != napi_function) return;

  napi_value argv[2] = {MakeInt32(wrap->env, exit_code), nullptr};
  if (has_signal) {
    const char* sig_name = SignalNameFromNumber(signal_code);
    if (sig_name != nullptr) {
      napi_create_string_utf8(wrap->env, sig_name, NAPI_AUTO_LENGTH, &argv[1]);
    } else {
      napi_get_null(wrap->env, &argv[1]);
    }
  } else {
    napi_get_null(wrap->env, &argv[1]);
  }
  napi_value ignored = nullptr;
  UbiMakeCallback(wrap->env, self, onexit, 2, argv, &ignored);
}

void OnProcessClose(uv_handle_t* handle) {
  auto* wrap = static_cast<ProcessWrap*>(handle->data);
  if (wrap == nullptr) return;
  wrap->process_closed = true;
  wrap->process_initialized = false;
  ReleaseWrapperRef(wrap);
  if (wrap->delete_on_close) {
    delete wrap;
  }
}

void OnProcessExit(uv_process_t* process, int64_t exit_status, int term_signal) {
  auto* wrap = static_cast<ProcessWrap*>(process->data);
  if (wrap == nullptr) return;
  wrap->alive = false;
  EmitOnExit(wrap, static_cast<int32_t>(exit_status), static_cast<int32_t>(term_signal), term_signal != 0);
  uv_handle_t* h = reinterpret_cast<uv_handle_t*>(process);
  if (!uv_is_closing(h)) {
    uv_close(h, OnProcessClose);
  }
}

void ProcessFinalize(napi_env env, void* data, void* /*hint*/) {
  auto* wrap = static_cast<ProcessWrap*>(data);
  if (wrap == nullptr) return;
  if (wrap->process_initialized) {
    uv_handle_t* h = reinterpret_cast<uv_handle_t*>(&wrap->process);
    if (!uv_is_closing(h)) {
      wrap->delete_on_close = true;
      uv_close(h, OnProcessClose);
    }
  }
  if (wrap->wrapper_ref != nullptr) napi_delete_reference(env, wrap->wrapper_ref);
  wrap->wrapper_ref = nullptr;
  if (wrap->process_initialized && !wrap->process_closed) return;
  delete wrap;
}

napi_value ProcessCtor(napi_env env, napi_callback_info info) {
  size_t argc = 0;
  napi_value self = nullptr;
  if (napi_get_cb_info(env, info, &argc, nullptr, &self, nullptr) != napi_ok || self == nullptr) {
    napi_value undefined = nullptr;
    napi_get_undefined(env, &undefined);
    return undefined;
  }
  auto* wrap = new ProcessWrap();
  wrap->env = env;
  napi_wrap(env, self, wrap, ProcessFinalize, nullptr, &wrap->wrapper_ref);
  SetPidProperty(env, self, 0);
  return self;
}

napi_value ProcessSpawn(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  napi_value self = nullptr;
  if (napi_get_cb_info(env, info, &argc, argv, &self, nullptr) != napi_ok || self == nullptr) {
    return MakeInt32(env, UV_EINVAL);
  }
  ProcessWrap* wrap = nullptr;
  if (napi_unwrap(env, self, reinterpret_cast<void**>(&wrap)) != napi_ok || wrap == nullptr) {
    return MakeInt32(env, UV_EINVAL);
  }
  if (wrap == nullptr || self == nullptr) return MakeInt32(env, UV_EINVAL);
  if (wrap->alive) return MakeInt32(env, UV_EINVAL);
  if (argc < 1 || argv[0] == nullptr) return MakeInt32(env, UV_EINVAL);

  bool use_real_async = true;
  napi_value real_async_value = nullptr;
  if (GetNamedValue(env, argv[0], "__ubiRealAsync", &real_async_value) && real_async_value != nullptr) {
    bool parsed = true;
    if (napi_get_value_bool(env, real_async_value, &parsed) == napi_ok) {
      use_real_async = parsed;
    }
  }
  if (!use_real_async) {
    wrap->real_async = false;
    wrap->pid = ++g_next_pid;
    wrap->alive = true;
    HoldWrapperRef(wrap);
    SetPidProperty(env, self, wrap->pid);
    return MakeInt32(env, 0);
  }

  std::string file;
  if (!GetStringProperty(env, argv[0], "file", &file) || file.empty()) {
    return MakeInt32(env, UV_EINVAL);
  }

  std::vector<std::string> args_storage;
  std::vector<char*> args;
  bool has_args = false;
  napi_value args_value = nullptr;
  if (napi_has_named_property(env, argv[0], "args", &has_args) == napi_ok && has_args &&
      napi_get_named_property(env, argv[0], "args", &args_value) == napi_ok && args_value != nullptr) {
    bool is_array = false;
    napi_is_array(env, args_value, &is_array);
    if (is_array) {
      uint32_t len = 0;
      napi_get_array_length(env, args_value, &len);
      args_storage.reserve(len);
      args.reserve(len + 1);
      for (uint32_t i = 0; i < len; i++) {
        napi_value item = nullptr;
        if (napi_get_element(env, args_value, i, &item) != napi_ok || item == nullptr) continue;
        size_t slen = 0;
        if (napi_get_value_string_utf8(env, item, nullptr, 0, &slen) != napi_ok) continue;
        std::string s(slen + 1, '\0');
        size_t copied = 0;
        if (napi_get_value_string_utf8(env, item, s.data(), s.size(), &copied) != napi_ok) continue;
        s.resize(copied);
        args_storage.push_back(std::move(s));
      }
    }
  }
  if (args_storage.empty()) args_storage.push_back(file);
  for (auto& s : args_storage) args.push_back(const_cast<char*>(s.c_str()));
  args.push_back(nullptr);

  std::vector<std::string> env_storage;
  std::vector<char*> envp;
  bool has_env_pairs = false;
  napi_value env_pairs = nullptr;
  if (napi_has_named_property(env, argv[0], "envPairs", &has_env_pairs) == napi_ok && has_env_pairs &&
      napi_get_named_property(env, argv[0], "envPairs", &env_pairs) == napi_ok && env_pairs != nullptr) {
    bool is_array = false;
    napi_is_array(env, env_pairs, &is_array);
    if (is_array) {
      uint32_t len = 0;
      napi_get_array_length(env, env_pairs, &len);
      env_storage.reserve(len);
      envp.reserve(len + 1);
      for (uint32_t i = 0; i < len; i++) {
        napi_value item = nullptr;
        if (napi_get_element(env, env_pairs, i, &item) != napi_ok || item == nullptr) continue;
        size_t slen = 0;
        if (napi_get_value_string_utf8(env, item, nullptr, 0, &slen) != napi_ok) continue;
        std::string s(slen + 1, '\0');
        size_t copied = 0;
        if (napi_get_value_string_utf8(env, item, s.data(), s.size(), &copied) != napi_ok) continue;
        s.resize(copied);
        env_storage.push_back(std::move(s));
      }
      for (auto& s : env_storage) envp.push_back(const_cast<char*>(s.c_str()));
      envp.push_back(nullptr);
    }
  }

  std::string cwd;
  bool has_cwd = GetStringProperty(env, argv[0], "cwd", &cwd);
  bool detached = IsTruthyProperty(env, argv[0], "detached");

  std::vector<uv_stdio_container_t> stdio;
  bool has_stdio = false;
  napi_value stdio_value = nullptr;
  if (napi_has_named_property(env, argv[0], "stdio", &has_stdio) == napi_ok && has_stdio &&
      napi_get_named_property(env, argv[0], "stdio", &stdio_value) == napi_ok && stdio_value != nullptr) {
    bool is_array = false;
    napi_is_array(env, stdio_value, &is_array);
    if (is_array) {
      uint32_t len = 0;
      napi_get_array_length(env, stdio_value, &len);
      stdio.resize(len);
      for (uint32_t i = 0; i < len; i++) {
        auto& entry = stdio[i];
        entry.flags = UV_IGNORE;
        entry.data.fd = -1;

        napi_value item = nullptr;
        if (napi_get_element(env, stdio_value, i, &item) != napi_ok || item == nullptr) continue;
        napi_valuetype t = napi_undefined;
        if (napi_typeof(env, item, &t) != napi_ok) continue;

        if (t == napi_string) {
          size_t slen = 0;
          if (napi_get_value_string_utf8(env, item, nullptr, 0, &slen) != napi_ok) continue;
          std::string s(slen + 1, '\0');
          size_t copied = 0;
          if (napi_get_value_string_utf8(env, item, s.data(), s.size(), &copied) != napi_ok) continue;
          s.resize(copied);
          if (s == "inherit") {
            entry.flags = UV_INHERIT_FD;
            entry.data.fd = static_cast<int>(i);
          } else if (s == "ignore" || s == "pipe" || s == "overlapped" || s == "ipc") {
            entry.flags = UV_IGNORE;
          }
          continue;
        }

        if (t == napi_number) {
          int32_t fd = -1;
          if (napi_get_value_int32(env, item, &fd) == napi_ok && fd >= 0) {
            entry.flags = UV_INHERIT_FD;
            entry.data.fd = fd;
          }
          continue;
        }

        if (t == napi_object) {
          napi_value type_value = nullptr;
          std::string type;
          if (GetNamedValue(env, item, "type", &type_value)) {
            size_t slen = 0;
            if (napi_get_value_string_utf8(env, type_value, nullptr, 0, &slen) == napi_ok) {
              std::string s(slen + 1, '\0');
              size_t copied = 0;
              if (napi_get_value_string_utf8(env, type_value, s.data(), s.size(), &copied) == napi_ok) {
                s.resize(copied);
                type = std::move(s);
              }
            }
          }

          if (type == "ignore") {
            entry.flags = UV_IGNORE;
            continue;
          }

          if (type == "fd" || type == "inherit") {
            int32_t fd = -1;
            if (GetInt32Property(env, item, "fd", &fd) && fd >= 0) {
              entry.flags = UV_INHERIT_FD;
              entry.data.fd = fd;
            }
            continue;
          }

          if (type == "pipe" || type == "overlapped" || type == "wrap") {
            napi_value handle_value = nullptr;
            if (!GetNamedValue(env, item, "handle", &handle_value)) {
              GetNamedValue(env, item, "_handle", &handle_value);
            }
            uv_stream_t* stream = UbiPipeWrapGetStream(env, handle_value);
            if (stream != nullptr) {
              if (type == "wrap") {
                entry.flags = UV_INHERIT_STREAM;
              } else {
                uv_stdio_flags f =
                    static_cast<uv_stdio_flags>(UV_CREATE_PIPE | UV_READABLE_PIPE | UV_WRITABLE_PIPE);
                if (type == "overlapped") {
                  f = static_cast<uv_stdio_flags>(f | UV_OVERLAPPED_PIPE);
                }
                entry.flags = f;
              }
              entry.data.stream = stream;
            }
            continue;
          }

          int32_t fd = -1;
          if (GetInt32Property(env, item, "fd", &fd) && fd >= 0) {
            entry.flags = UV_INHERIT_FD;
            entry.data.fd = fd;
          }
          continue;
        }
      }
    }
  }

  if (stdio.empty()) {
    stdio.resize(3);
    stdio[0].flags = UV_INHERIT_FD;
    stdio[0].data.fd = 0;
    stdio[1].flags = UV_INHERIT_FD;
    stdio[1].data.fd = 1;
    stdio[2].flags = UV_INHERIT_FD;
    stdio[2].data.fd = 2;
  }

  uv_process_options_t options{};
  options.file = file.c_str();
  options.args = args.data();
  options.exit_cb = OnProcessExit;
  options.flags = detached ? UV_PROCESS_DETACHED : 0;
  if (has_cwd) options.cwd = cwd.c_str();
  if (!envp.empty()) options.env = envp.data();
  options.stdio_count = static_cast<int>(stdio.size());
  options.stdio = stdio.data();

  wrap->process = {};
  wrap->process.data = wrap;
  int rc = uv_spawn(uv_default_loop(), &wrap->process, &options);
  if (rc != 0) {
    wrap->process_initialized = false;
    wrap->process_closed = false;
    return MakeInt32(env, rc);
  }

  wrap->process_initialized = true;
  wrap->process_closed = false;
  wrap->real_async = true;
  wrap->pid = static_cast<int32_t>(wrap->process.pid);
  wrap->alive = true;
  uv_ref(reinterpret_cast<uv_handle_t*>(&wrap->process));
  HoldWrapperRef(wrap);
  SetPidProperty(env, self, wrap->pid);
  return MakeInt32(env, 0);
}

napi_value ProcessKill(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  napi_value self = nullptr;
  if (napi_get_cb_info(env, info, &argc, argv, &self, nullptr) != napi_ok || self == nullptr) {
    return MakeInt32(env, UV_EINVAL);
  }

  ProcessWrap* wrap = nullptr;
  if (napi_unwrap(env, self, reinterpret_cast<void**>(&wrap)) != napi_ok || wrap == nullptr) {
    return MakeInt32(env, UV_EINVAL);
  }
  if (!wrap->alive) return MakeInt32(env, UV_ESRCH);

  int32_t signal = 0;
  bool has_signal = false;
  if (argc >= 1 && argv[0] != nullptr) {
    napi_valuetype t = napi_undefined;
    if (napi_typeof(env, argv[0], &t) == napi_ok) {
      if (t == napi_number && napi_get_value_int32(env, argv[0], &signal) == napi_ok) {
        has_signal = true;
      } else if (t == napi_string) {
        size_t len = 0;
        if (napi_get_value_string_utf8(env, argv[0], nullptr, 0, &len) == napi_ok) {
          std::string sig_name(len + 1, '\0');
          size_t copied = 0;
          if (napi_get_value_string_utf8(env, argv[0], sig_name.data(), sig_name.size(), &copied) == napi_ok) {
            sig_name.resize(copied);
            int sig = SignalFromName(sig_name);
            if (sig > 0) {
              signal = sig;
              has_signal = true;
            }
          }
        }
      }
    }
  }
  if (has_signal && signal == 0) return MakeInt32(env, 0);

  if (wrap->real_async && wrap->process_initialized) {
    int kill_signal = has_signal ? signal : SIGTERM;
    int rc = uv_process_kill(&wrap->process, kill_signal);
    if (rc == UV_ESRCH) wrap->alive = false;
    return MakeInt32(env, rc);
  }

  wrap->alive = false;
  EmitOnExit(wrap, 0, signal, has_signal);
  ReleaseWrapperRef(wrap);

  return MakeInt32(env, 0);
}

napi_value ProcessClose(napi_env env, napi_callback_info info) {
  ProcessWrap* wrap = UnwrapProcess(env, info, nullptr);
  if (wrap != nullptr) {
    wrap->alive = false;
    if (wrap->process_initialized) {
      uv_handle_t* h = reinterpret_cast<uv_handle_t*>(&wrap->process);
      if (!uv_is_closing(h)) {
        uv_close(h, OnProcessClose);
      }
    } else {
      ReleaseWrapperRef(wrap);
    }
  }
  napi_value undefined = nullptr;
  napi_get_undefined(env, &undefined);
  return undefined;
}

napi_value ProcessRef(napi_env env, napi_callback_info info) {
  ProcessWrap* wrap = UnwrapProcess(env, info, nullptr);
  if (wrap != nullptr && wrap->process_initialized) {
    uv_ref(reinterpret_cast<uv_handle_t*>(&wrap->process));
  }
  napi_value undefined = nullptr;
  napi_get_undefined(env, &undefined);
  return undefined;
}

napi_value ProcessUnref(napi_env env, napi_callback_info info) {
  ProcessWrap* wrap = UnwrapProcess(env, info, nullptr);
  if (wrap != nullptr && wrap->process_initialized) {
    uv_unref(reinterpret_cast<uv_handle_t*>(&wrap->process));
  }
  napi_value undefined = nullptr;
  napi_get_undefined(env, &undefined);
  return undefined;
}

}  // namespace

napi_value UbiInstallProcessWrapBinding(napi_env env) {
  if (env == nullptr) return nullptr;
  napi_property_descriptor methods[] = {
      {"spawn", nullptr, ProcessSpawn, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"kill", nullptr, ProcessKill, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"close", nullptr, ProcessClose, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"ref", nullptr, ProcessRef, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"unref", nullptr, ProcessUnref, nullptr, nullptr, nullptr, napi_default, nullptr},
  };

  napi_value process_ctor = nullptr;
  if (napi_define_class(env,
                        "Process",
                        NAPI_AUTO_LENGTH,
                        ProcessCtor,
                        nullptr,
                        sizeof(methods) / sizeof(methods[0]),
                        methods,
                        &process_ctor) != napi_ok ||
      process_ctor == nullptr) {
    return nullptr;
  }

  napi_value binding = nullptr;
  if (napi_create_object(env, &binding) != napi_ok || binding == nullptr) return nullptr;
  napi_set_named_property(env, binding, "Process", process_ctor);

  return binding;
}
