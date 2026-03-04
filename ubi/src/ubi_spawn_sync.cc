#include "ubi_spawn_sync.h"

#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <uv.h>

namespace {

struct SpawnOptions {
  std::string file;
  std::vector<std::string> args;
  std::string cwd;
  int64_t timeout_ms = 0;
  int64_t max_buffer = 1024 * 1024;
  int32_t kill_signal = SIGTERM;
  std::vector<std::string> env_pairs;
  std::vector<uint8_t> stdin_input;
};

struct SpawnSyncRunner {
  uv_loop_t loop{};
  bool loop_initialized = false;

  uv_process_t process{};
  bool process_spawned = false;
  bool process_exited = false;
  int64_t exit_status = -1;
  int term_signal = 0;

  uv_pipe_t stdin_pipe{};
  uv_pipe_t stdout_pipe{};
  uv_pipe_t stderr_pipe{};
  bool stdin_initialized = false;
  bool stdout_initialized = false;
  bool stderr_initialized = false;

  uv_timer_t timer{};
  bool timer_initialized = false;

  uv_write_t stdin_write{};
  uv_shutdown_t stdin_shutdown{};
  bool stdin_shutdown_pending = false;

  uv_stdio_container_t stdio[3]{};

  std::vector<uint8_t> stdin_storage;
  uv_buf_t stdin_buf{};

  std::vector<uint8_t> out_stdout;
  std::vector<uint8_t> out_stderr;

  int kill_signal = SIGTERM;
  int64_t max_buffer = 1024 * 1024;
  int64_t buffered_output_size = 0;

  bool timed_out = false;
  bool max_buffer_exceeded = false;
  bool kill_attempted = false;

  int error = 0;
  int pipe_error = 0;
};

bool GetNamedProperty(napi_env env, napi_value obj, const char* name, napi_value* out) {
  if (out == nullptr) return false;
  *out = nullptr;
  bool has = false;
  if (napi_has_named_property(env, obj, name, &has) != napi_ok || !has) return false;
  return napi_get_named_property(env, obj, name, out) == napi_ok && *out != nullptr;
}

std::string ValueToUtf8(napi_env env, napi_value value) {
  size_t len = 0;
  if (napi_get_value_string_utf8(env, value, nullptr, 0, &len) != napi_ok) return "";
  std::string out(len + 1, '\0');
  size_t copied = 0;
  if (napi_get_value_string_utf8(env, value, out.data(), out.size(), &copied) != napi_ok) return "";
  out.resize(copied);
  return out;
}

bool ParseSpawnOptions(napi_env env, napi_value value, SpawnOptions* out) {
  if (out == nullptr) return false;
  napi_valuetype t = napi_undefined;
  if (napi_typeof(env, value, &t) != napi_ok || t != napi_object) return false;

  napi_value file_val = nullptr;
  if (GetNamedProperty(env, value, "file", &file_val)) {
    out->file = ValueToUtf8(env, file_val);
  }
  if (out->file.empty()) return false;

  napi_value args_val = nullptr;
  bool has_args = GetNamedProperty(env, value, "args", &args_val);
  bool is_array = false;
  if (has_args && napi_is_array(env, args_val, &is_array) == napi_ok && is_array) {
    uint32_t len = 0;
    if (napi_get_array_length(env, args_val, &len) == napi_ok) {
      out->args.reserve(static_cast<size_t>(len));
      for (uint32_t i = 0; i < len; ++i) {
        napi_value elem = nullptr;
        if (napi_get_element(env, args_val, i, &elem) != napi_ok || elem == nullptr) continue;
        out->args.push_back(ValueToUtf8(env, elem));
      }
    }
  }
  if (out->args.empty()) {
    out->args.push_back(out->file);
  }

  napi_value cwd_val = nullptr;
  if (GetNamedProperty(env, value, "cwd", &cwd_val)) {
    napi_valuetype cwd_t = napi_undefined;
    if (napi_typeof(env, cwd_val, &cwd_t) == napi_ok && cwd_t == napi_string) {
      out->cwd = ValueToUtf8(env, cwd_val);
    }
  }

  napi_value timeout_val = nullptr;
  if (GetNamedProperty(env, value, "timeout", &timeout_val)) {
    napi_valuetype timeout_t = napi_undefined;
    if (napi_typeof(env, timeout_val, &timeout_t) == napi_ok &&
        (timeout_t == napi_number || timeout_t == napi_bigint)) {
      int64_t timeout = 0;
      if (napi_get_value_int64(env, timeout_val, &timeout) == napi_ok && timeout > 0) {
        out->timeout_ms = timeout;
      }
    }
  }

  napi_value max_buffer_val = nullptr;
  if (GetNamedProperty(env, value, "maxBuffer", &max_buffer_val)) {
    napi_valuetype mb_t = napi_undefined;
    if (napi_typeof(env, max_buffer_val, &mb_t) == napi_ok && mb_t == napi_number) {
      double mb = 0;
      if (napi_get_value_double(env, max_buffer_val, &mb) == napi_ok && mb >= 0) {
        if (mb > static_cast<double>(INT64_MAX)) out->max_buffer = INT64_MAX;
        else out->max_buffer = static_cast<int64_t>(mb);
      }
    }
  }

  napi_value env_pairs_val = nullptr;
  bool has_env_pairs = GetNamedProperty(env, value, "envPairs", &env_pairs_val);
  bool env_is_array = false;
  if (has_env_pairs && napi_is_array(env, env_pairs_val, &env_is_array) == napi_ok && env_is_array) {
    uint32_t len = 0;
    if (napi_get_array_length(env, env_pairs_val, &len) == napi_ok) {
      out->env_pairs.reserve(static_cast<size_t>(len));
      for (uint32_t i = 0; i < len; ++i) {
        napi_value elem = nullptr;
        if (napi_get_element(env, env_pairs_val, i, &elem) != napi_ok || elem == nullptr) continue;
        out->env_pairs.push_back(ValueToUtf8(env, elem));
      }
    }
  }

  napi_value kill_signal_val = nullptr;
  if (GetNamedProperty(env, value, "killSignal", &kill_signal_val)) {
    napi_valuetype ks_t = napi_undefined;
    if (napi_typeof(env, kill_signal_val, &ks_t) == napi_ok && ks_t == napi_number) {
      int32_t ks = SIGTERM;
      if (napi_get_value_int32(env, kill_signal_val, &ks) == napi_ok && ks > 0) {
        out->kill_signal = ks;
      }
    }
  }

  napi_value stdio_val = nullptr;
  bool has_stdio = GetNamedProperty(env, value, "stdio", &stdio_val);
  bool stdio_is_array = false;
  if (has_stdio && napi_is_array(env, stdio_val, &stdio_is_array) == napi_ok && stdio_is_array) {
    napi_value stdin_desc = nullptr;
    if (napi_get_element(env, stdio_val, 0, &stdin_desc) == napi_ok && stdin_desc != nullptr) {
      napi_value input_val = nullptr;
      if (GetNamedProperty(env, stdin_desc, "input", &input_val)) {
        bool is_buffer = false;
        if (napi_is_buffer(env, input_val, &is_buffer) == napi_ok && is_buffer) {
          void* ptr = nullptr;
          size_t len = 0;
          if (napi_get_buffer_info(env, input_val, &ptr, &len) == napi_ok && ptr != nullptr && len > 0) {
            const uint8_t* bytes = static_cast<const uint8_t*>(ptr);
            out->stdin_input.assign(bytes, bytes + len);
          }
        } else {
          bool is_typedarray = false;
          if (napi_is_typedarray(env, input_val, &is_typedarray) == napi_ok && is_typedarray) {
            napi_typedarray_type ta_type;
            size_t element_len = 0;
            void* raw = nullptr;
            napi_value ab = nullptr;
            size_t byte_offset = 0;
            if (napi_get_typedarray_info(env, input_val, &ta_type, &element_len, &raw, &ab, &byte_offset) == napi_ok &&
                raw != nullptr) {
              size_t bytes = element_len;
              switch (ta_type) {
                case napi_uint16_array:
                case napi_int16_array: bytes *= 2; break;
                case napi_uint32_array:
                case napi_int32_array:
                case napi_float32_array: bytes *= 4; break;
                case napi_float64_array:
                case napi_bigint64_array:
                case napi_biguint64_array: bytes *= 8; break;
                default: break;
              }
              const uint8_t* data = static_cast<const uint8_t*>(raw);
              out->stdin_input.assign(data, data + bytes);
            }
          }
        }
      }
    }
  }

  return true;
}

const char* SignalName(int signum) {
  switch (signum) {
    case SIGTERM: return "SIGTERM";
    case SIGKILL: return "SIGKILL";
    case SIGINT: return "SIGINT";
    case SIGABRT: return "SIGABRT";
    case SIGQUIT: return "SIGQUIT";
    case SIGSEGV: return "SIGSEGV";
    case SIGILL: return "SIGILL";
#if defined(SIGTRAP)
    case SIGTRAP: return "SIGTRAP";
#endif
#if defined(SIGBUS)
    case SIGBUS: return "SIGBUS";
#endif
    default: return nullptr;
  }
}

void UvCloseNoop(uv_handle_t* /*handle*/) {}

void SetErrorIfUnset(SpawnSyncRunner* runner, int error) {
  if (runner == nullptr || error == 0) return;
  if (runner->error == 0) runner->error = error;
}

void SetPipeErrorIfUnset(SpawnSyncRunner* runner, int error) {
  if (runner == nullptr || error == 0) return;
  if (runner->pipe_error == 0) runner->pipe_error = error;
}

void CloseHandleIfNeeded(uv_handle_t* handle) {
  if (handle == nullptr) return;
  if (!uv_is_closing(handle)) uv_close(handle, UvCloseNoop);
}

void WalkAndCloseHandle(uv_handle_t* handle, void* /*arg*/) {
  if (handle == nullptr) return;
  if (!uv_is_closing(handle)) {
    uv_close(handle, UvCloseNoop);
  }
}

void CloseStdioHandles(SpawnSyncRunner* runner) {
  if (runner == nullptr) return;
  if (runner->stdin_initialized) {
    CloseHandleIfNeeded(reinterpret_cast<uv_handle_t*>(&runner->stdin_pipe));
  }
  if (runner->stdout_initialized) {
    CloseHandleIfNeeded(reinterpret_cast<uv_handle_t*>(&runner->stdout_pipe));
  }
  if (runner->stderr_initialized) {
    CloseHandleIfNeeded(reinterpret_cast<uv_handle_t*>(&runner->stderr_pipe));
  }
}

void CloseTimerIfNeeded(SpawnSyncRunner* runner) {
  if (runner == nullptr || !runner->timer_initialized) return;
  CloseHandleIfNeeded(reinterpret_cast<uv_handle_t*>(&runner->timer));
}

void KillAndClose(SpawnSyncRunner* runner) {
  if (runner == nullptr) return;
  if (!runner->kill_attempted) {
    runner->kill_attempted = true;
    if (runner->process_spawned && !runner->process_exited) {
      int rc = uv_process_kill(&runner->process, runner->kill_signal);
      if (rc < 0 && rc != UV_ESRCH) {
        SetErrorIfUnset(runner, rc);
        (void)uv_process_kill(&runner->process, SIGKILL);
      }
    }
  }
  CloseStdioHandles(runner);
  CloseTimerIfNeeded(runner);
}

void StartStdinShutdown(SpawnSyncRunner* runner);

void StdinShutdownCallback(uv_shutdown_t* req, int status) {
  if (req == nullptr) return;
  auto* runner = static_cast<SpawnSyncRunner*>(req->data);
  if (runner == nullptr) return;
  runner->stdin_shutdown_pending = false;
  if (status < 0 && status != UV_ENOTCONN) {
    SetPipeErrorIfUnset(runner, status);
  }
  if (runner->stdin_initialized) {
    CloseHandleIfNeeded(reinterpret_cast<uv_handle_t*>(&runner->stdin_pipe));
  }
}

void StartStdinShutdown(SpawnSyncRunner* runner) {
  if (runner == nullptr || !runner->stdin_initialized) return;
  uv_handle_t* handle = reinterpret_cast<uv_handle_t*>(&runner->stdin_pipe);
  if (uv_is_closing(handle) || runner->stdin_shutdown_pending) return;

  runner->stdin_shutdown.data = runner;
  runner->stdin_shutdown_pending = true;
  int rc = uv_shutdown(&runner->stdin_shutdown,
                       reinterpret_cast<uv_stream_t*>(&runner->stdin_pipe),
                       StdinShutdownCallback);
  if (rc == UV_ENOTCONN) {
    runner->stdin_shutdown_pending = false;
    CloseHandleIfNeeded(handle);
    return;
  }
  if (rc < 0) {
    runner->stdin_shutdown_pending = false;
    SetPipeErrorIfUnset(runner, rc);
    CloseHandleIfNeeded(handle);
  }
}

void StdinWriteCallback(uv_write_t* req, int status) {
  if (req == nullptr) return;
  auto* runner = static_cast<SpawnSyncRunner*>(req->data);
  if (runner == nullptr) return;
  if (status < 0) {
    SetPipeErrorIfUnset(runner, status);
  }
  StartStdinShutdown(runner);
}

void AllocReadBuffer(uv_handle_t* /*handle*/, size_t suggested_size, uv_buf_t* buf) {
  if (buf == nullptr) return;
  char* base = static_cast<char*>(std::malloc(suggested_size));
  if (base == nullptr) {
    *buf = uv_buf_init(nullptr, 0);
    return;
  }
  *buf = uv_buf_init(base, static_cast<unsigned int>(suggested_size));
}

void OnProcessExit(uv_process_t* process, int64_t exit_status, int term_signal) {
  if (process == nullptr) return;
  auto* runner = static_cast<SpawnSyncRunner*>(process->data);
  if (runner == nullptr) return;

  runner->process_exited = true;
  runner->exit_status = exit_status;
  runner->term_signal = term_signal;

  CloseHandleIfNeeded(reinterpret_cast<uv_handle_t*>(process));
  CloseTimerIfNeeded(runner);
}

void OnKillTimer(uv_timer_t* timer) {
  if (timer == nullptr) return;
  auto* runner = static_cast<SpawnSyncRunner*>(timer->data);
  if (runner == nullptr) return;

  runner->timed_out = true;
  SetErrorIfUnset(runner, UV_ETIMEDOUT);
  KillAndClose(runner);
}

void OnPipeRead(uv_stream_t* stream, ssize_t nread, const uv_buf_t* buf) {
  auto* runner = stream == nullptr ? nullptr : static_cast<SpawnSyncRunner*>(stream->data);

  if (nread > 0 && runner != nullptr && buf != nullptr && buf->base != nullptr) {
    const uint8_t* begin = reinterpret_cast<const uint8_t*>(buf->base);
    const uint8_t* end = begin + static_cast<size_t>(nread);

    if (stream == reinterpret_cast<uv_stream_t*>(&runner->stdout_pipe)) {
      runner->out_stdout.insert(runner->out_stdout.end(), begin, end);
    } else if (stream == reinterpret_cast<uv_stream_t*>(&runner->stderr_pipe)) {
      runner->out_stderr.insert(runner->out_stderr.end(), begin, end);
    }

    runner->buffered_output_size += nread;
    if (runner->max_buffer > 0 && runner->buffered_output_size > runner->max_buffer) {
      runner->max_buffer_exceeded = true;
      SetErrorIfUnset(runner, UV_ENOBUFS);
      KillAndClose(runner);
    }
  } else if (nread == UV_EOF) {
    if (stream != nullptr) {
      CloseHandleIfNeeded(reinterpret_cast<uv_handle_t*>(stream));
    }
  } else if (nread < 0) {
    if (runner != nullptr) {
      SetPipeErrorIfUnset(runner, static_cast<int>(nread));
    }
    if (stream != nullptr) {
      CloseHandleIfNeeded(reinterpret_cast<uv_handle_t*>(stream));
    }
  }

  if (buf != nullptr && buf->base != nullptr) {
    std::free(buf->base);
  }
}

int GetFinalError(const SpawnSyncRunner& runner) {
  if (runner.error != 0) return runner.error;
  return runner.pipe_error;
}

void CleanupRunner(SpawnSyncRunner* runner) {
  if (runner == nullptr || !runner->loop_initialized) return;

  if (runner->process_spawned) {
    CloseHandleIfNeeded(reinterpret_cast<uv_handle_t*>(&runner->process));
  }
  CloseStdioHandles(runner);
  CloseTimerIfNeeded(runner);

  // Drain close callbacks and force-close anything still registered on the loop
  // so file descriptors are always released before returning.
  int close_rc = uv_loop_close(&runner->loop);
  while (close_rc == UV_EBUSY) {
    uv_walk(&runner->loop, WalkAndCloseHandle, nullptr);
    (void)uv_run(&runner->loop, UV_RUN_DEFAULT);
    close_rc = uv_loop_close(&runner->loop);
  }
  runner->loop_initialized = false;
}

napi_value SpawnSync(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);

  SpawnOptions options;
  if (argc < 1 || argv[0] == nullptr || !ParseSpawnOptions(env, argv[0], &options)) {
    napi_value out = nullptr;
    napi_create_object(env, &out);
    napi_value err = nullptr;
    napi_create_int32(env, -EINVAL, &err);
    napi_set_named_property(env, out, "error", err);
    return out;
  }

  SpawnSyncRunner runner;
  runner.kill_signal = options.kill_signal;
  runner.max_buffer = options.max_buffer;

  int rc = uv_loop_init(&runner.loop);
  if (rc < 0) {
    napi_value out = nullptr;
    napi_create_object(env, &out);
    napi_value err = nullptr;
    napi_create_int32(env, rc, &err);
    napi_set_named_property(env, out, "error", err);
    return out;
  }
  runner.loop_initialized = true;

  rc = uv_pipe_init(&runner.loop, &runner.stdin_pipe, 0);
  if (rc >= 0) {
    runner.stdin_initialized = true;
    runner.stdin_pipe.data = &runner;
  } else {
    SetErrorIfUnset(&runner, rc);
  }

  if (rc >= 0) {
    rc = uv_pipe_init(&runner.loop, &runner.stdout_pipe, 0);
    if (rc >= 0) {
      runner.stdout_initialized = true;
      runner.stdout_pipe.data = &runner;
    } else {
      SetErrorIfUnset(&runner, rc);
    }
  }

  if (rc >= 0) {
    rc = uv_pipe_init(&runner.loop, &runner.stderr_pipe, 0);
    if (rc >= 0) {
      runner.stderr_initialized = true;
      runner.stderr_pipe.data = &runner;
    } else {
      SetErrorIfUnset(&runner, rc);
    }
  }

  std::vector<char*> exec_args;
  exec_args.reserve(options.args.size() + 1);
  for (std::string& arg : options.args) {
    exec_args.push_back(const_cast<char*>(arg.c_str()));
  }
  exec_args.push_back(nullptr);

  std::vector<char*> exec_env;
  if (!options.env_pairs.empty()) {
    exec_env.reserve(options.env_pairs.size() + 1);
    for (std::string& kv : options.env_pairs) {
      exec_env.push_back(const_cast<char*>(kv.c_str()));
    }
    exec_env.push_back(nullptr);
  }

  if (GetFinalError(runner) == 0) {
    uv_process_options_t uv_options{};
    uv_options.file = options.file.c_str();
    uv_options.args = exec_args.data();
    uv_options.exit_cb = OnProcessExit;
    if (!options.cwd.empty()) uv_options.cwd = options.cwd.c_str();
    if (!exec_env.empty()) uv_options.env = exec_env.data();

    runner.stdio[0].flags = static_cast<uv_stdio_flags>(UV_CREATE_PIPE | UV_READABLE_PIPE);
    runner.stdio[0].data.stream = reinterpret_cast<uv_stream_t*>(&runner.stdin_pipe);
    runner.stdio[1].flags = static_cast<uv_stdio_flags>(UV_CREATE_PIPE | UV_WRITABLE_PIPE);
    runner.stdio[1].data.stream = reinterpret_cast<uv_stream_t*>(&runner.stdout_pipe);
    runner.stdio[2].flags = static_cast<uv_stdio_flags>(UV_CREATE_PIPE | UV_WRITABLE_PIPE);
    runner.stdio[2].data.stream = reinterpret_cast<uv_stream_t*>(&runner.stderr_pipe);

    uv_options.stdio_count = 3;
    uv_options.stdio = runner.stdio;

    rc = uv_spawn(&runner.loop, &runner.process, &uv_options);
    if (rc < 0) {
      SetErrorIfUnset(&runner, rc);
    } else {
      runner.process_spawned = true;
      runner.process.data = &runner;
    }
  }

  if (runner.process_spawned) {
    rc = uv_read_start(reinterpret_cast<uv_stream_t*>(&runner.stdout_pipe), AllocReadBuffer, OnPipeRead);
    if (rc < 0) {
      SetPipeErrorIfUnset(&runner, rc);
      KillAndClose(&runner);
    }

    rc = uv_read_start(reinterpret_cast<uv_stream_t*>(&runner.stderr_pipe), AllocReadBuffer, OnPipeRead);
    if (rc < 0) {
      SetPipeErrorIfUnset(&runner, rc);
      KillAndClose(&runner);
    }

    if (!options.stdin_input.empty()) {
      runner.stdin_storage = options.stdin_input;
      runner.stdin_buf = uv_buf_init(
          reinterpret_cast<char*>(runner.stdin_storage.data()),
          static_cast<unsigned int>(runner.stdin_storage.size()));
      runner.stdin_write.data = &runner;
      rc = uv_write(&runner.stdin_write,
                    reinterpret_cast<uv_stream_t*>(&runner.stdin_pipe),
                    &runner.stdin_buf,
                    1,
                    StdinWriteCallback);
      if (rc < 0) {
        SetPipeErrorIfUnset(&runner, rc);
        StartStdinShutdown(&runner);
      }
    } else {
      StartStdinShutdown(&runner);
    }

    if (options.timeout_ms > 0) {
      rc = uv_timer_init(&runner.loop, &runner.timer);
      if (rc < 0) {
        SetErrorIfUnset(&runner, rc);
      } else {
        runner.timer_initialized = true;
        runner.timer.data = &runner;
        rc = uv_timer_start(&runner.timer,
                            OnKillTimer,
                            static_cast<uint64_t>(options.timeout_ms),
                            0);
        if (rc < 0) {
          SetErrorIfUnset(&runner, rc);
          CloseTimerIfNeeded(&runner);
        }
      }
    }
  }

  if (GetFinalError(runner) != 0 && runner.process_spawned) {
    KillAndClose(&runner);
  }

  (void)uv_run(&runner.loop, UV_RUN_DEFAULT);

  napi_value result = nullptr;
  napi_create_object(env, &result);

  napi_value pid_value = nullptr;
  int32_t pid = runner.process_spawned ? static_cast<int32_t>(runner.process.pid) : 0;
  napi_create_int32(env, pid, &pid_value);
  napi_set_named_property(env, result, "pid", pid_value);

  napi_value output = nullptr;
  napi_create_array_with_length(env, 3, &output);
  napi_value null_value = nullptr;
  napi_get_null(env, &null_value);
  napi_set_element(env, output, 0, null_value);

  napi_value stdout_val = nullptr;
  napi_create_buffer_copy(env,
                          runner.out_stdout.size(),
                          runner.out_stdout.empty() ? nullptr : runner.out_stdout.data(),
                          nullptr,
                          &stdout_val);
  napi_set_element(env, output, 1, stdout_val);

  napi_value stderr_val = nullptr;
  napi_create_buffer_copy(env,
                          runner.out_stderr.size(),
                          runner.out_stderr.empty() ? nullptr : runner.out_stderr.data(),
                          nullptr,
                          &stderr_val);
  napi_set_element(env, output, 2, stderr_val);
  napi_set_named_property(env, result, "output", output);

  if (runner.process_exited) {
    if (runner.term_signal > 0) {
      napi_set_named_property(env, result, "status", null_value);
      const char* sig_name = SignalName(runner.term_signal);
      if (sig_name != nullptr) {
        napi_value signal_value = nullptr;
        napi_create_string_utf8(env, sig_name, NAPI_AUTO_LENGTH, &signal_value);
        napi_set_named_property(env, result, "signal", signal_value);
      } else {
        napi_set_named_property(env, result, "signal", null_value);
      }
    } else {
      napi_value status_value = nullptr;
      napi_create_int32(env, static_cast<int32_t>(runner.exit_status), &status_value);
      napi_set_named_property(env, result, "status", status_value);
      napi_set_named_property(env, result, "signal", null_value);
    }
  } else {
    napi_set_named_property(env, result, "status", null_value);
    napi_set_named_property(env, result, "signal", null_value);
  }

  const int final_error = GetFinalError(runner);
  if (final_error != 0) {
    napi_value error_value = nullptr;
    napi_create_int32(env, final_error, &error_value);
    napi_set_named_property(env, result, "error", error_value);
  }

  CleanupRunner(&runner);
  return result;
}

void SetMethod(napi_env env, napi_value obj, const char* name, napi_callback cb) {
  napi_value fn = nullptr;
  if (napi_create_function(env, name, NAPI_AUTO_LENGTH, cb, nullptr, &fn) != napi_ok || fn == nullptr) {
    return;
  }
  napi_set_named_property(env, obj, name, fn);
}

}  // namespace

napi_value UbiInstallSpawnSyncBinding(napi_env env) {
  if (env == nullptr) return nullptr;
  napi_value binding = nullptr;
  if (napi_create_object(env, &binding) != napi_ok || binding == nullptr) return nullptr;
  SetMethod(env, binding, "spawn", SpawnSync);
  return binding;
}
