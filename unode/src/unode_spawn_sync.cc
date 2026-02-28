#include "unode_spawn_sync.h"

#include <cerrno>
#include <csignal>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <cstring>
#include <string>
#include <vector>

#include <fcntl.h>
#include <sys/select.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

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

void SetNonBlocking(int fd) {
  int flags = fcntl(fd, F_GETFL, 0);
  if (flags >= 0) {
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
  }
}

void AppendFromFd(int fd, std::vector<uint8_t>* out, bool* open) {
  if (fd < 0 || out == nullptr || open == nullptr || !*open) return;
  uint8_t buf[4096];
  while (true) {
    const ssize_t n = read(fd, buf, sizeof(buf));
    if (n > 0) {
      out->insert(out->end(), buf, buf + n);
      continue;
    }
    if (n == 0) {
      *open = false;
      close(fd);
      return;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK) return;
    *open = false;
    close(fd);
    return;
  }
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

  int stdin_pipe[2] = {-1, -1};
  int stdout_pipe[2] = {-1, -1};
  int stderr_pipe[2] = {-1, -1};
  int exec_err_pipe[2] = {-1, -1};
  if (pipe(stdin_pipe) != 0 || pipe(stdout_pipe) != 0 || pipe(stderr_pipe) != 0 || pipe(exec_err_pipe) != 0) {
    napi_value out = nullptr;
    napi_create_object(env, &out);
    napi_value err = nullptr;
    napi_create_int32(env, -errno, &err);
    napi_set_named_property(env, out, "error", err);
    return out;
  }

  pid_t pid = fork();
  if (pid < 0) {
    const int err_no = errno;
    close(stdin_pipe[0]); close(stdin_pipe[1]);
    close(stdout_pipe[0]); close(stdout_pipe[1]);
    close(stderr_pipe[0]); close(stderr_pipe[1]);
    close(exec_err_pipe[0]); close(exec_err_pipe[1]);
    napi_value out = nullptr;
    napi_create_object(env, &out);
    napi_value err = nullptr;
    napi_create_int32(env, -err_no, &err);
    napi_set_named_property(env, out, "error", err);
    return out;
  }

  if (pid == 0) {
    close(stdin_pipe[1]);
    close(stdout_pipe[0]);
    close(stderr_pipe[0]);
    close(exec_err_pipe[0]);
    fcntl(exec_err_pipe[1], F_SETFD, FD_CLOEXEC);

    if (!options.cwd.empty() && chdir(options.cwd.c_str()) != 0) {
      const int chdir_errno = errno;
      (void)write(exec_err_pipe[1], &chdir_errno, sizeof(chdir_errno));
      _exit(127);
    }

    dup2(stdin_pipe[0], STDIN_FILENO);
    dup2(stdout_pipe[1], STDOUT_FILENO);
    dup2(stderr_pipe[1], STDERR_FILENO);
    close(stdin_pipe[0]);
    close(stdout_pipe[1]);
    close(stderr_pipe[1]);

    std::vector<char*> exec_argv;
    exec_argv.reserve(options.args.size() + 1);
    for (std::string& arg : options.args) {
      exec_argv.push_back(const_cast<char*>(arg.c_str()));
    }
    exec_argv.push_back(nullptr);

    if (options.env_pairs.empty()) {
      execvp(options.file.c_str(), exec_argv.data());
    } else {
      std::vector<std::string> env_storage = options.env_pairs;
      std::vector<char*> envp;
      envp.reserve(env_storage.size() + 1);
      for (std::string& kv : env_storage) envp.push_back(const_cast<char*>(kv.c_str()));
      envp.push_back(nullptr);

      if (options.file.find('/') != std::string::npos) {
        execve(options.file.c_str(), exec_argv.data(), envp.data());
      } else {
        std::string path_value = "/usr/bin:/bin:/usr/sbin:/sbin";
        for (const std::string& kv : env_storage) {
          if (kv.rfind("PATH=", 0) == 0) {
            path_value = kv.substr(5);
            break;
          }
        }
        size_t start = 0;
        while (true) {
          size_t end = path_value.find(':', start);
          std::string dir = (end == std::string::npos) ? path_value.substr(start) : path_value.substr(start, end - start);
          if (dir.empty()) dir = ".";
          std::string candidate = dir + "/" + options.file;
          execve(candidate.c_str(), exec_argv.data(), envp.data());
          if (end == std::string::npos) break;
          start = end + 1;
        }
      }
    }
    const int exec_errno = errno;
    (void)write(exec_err_pipe[1], &exec_errno, sizeof(exec_errno));
    _exit(127);
  }

  close(stdin_pipe[0]);
  close(stdout_pipe[1]);
  close(stderr_pipe[1]);
  close(exec_err_pipe[1]);

  if (!options.stdin_input.empty()) {
    const uint8_t* ptr = options.stdin_input.data();
    size_t left = options.stdin_input.size();
    while (left > 0) {
      const ssize_t wrote = write(stdin_pipe[1], ptr, left);
      if (wrote > 0) {
        ptr += wrote;
        left -= static_cast<size_t>(wrote);
      } else if (wrote < 0 && errno == EINTR) {
        continue;
      } else {
        break;
      }
    }
  }
  close(stdin_pipe[1]);
  SetNonBlocking(stdout_pipe[0]);
  SetNonBlocking(stderr_pipe[0]);

  bool stdout_open = true;
  bool stderr_open = true;
  bool child_running = true;
  bool timed_out = false;
  bool max_buffer_exceeded = false;
  int status = 0;
  int spawn_errno = 0;
  std::vector<uint8_t> out_stdout;
  std::vector<uint8_t> out_stderr;

  const auto start_time = std::chrono::steady_clock::now();
  while (stdout_open || stderr_open || child_running) {
    if (child_running) {
      pid_t waited = waitpid(pid, &status, WNOHANG);
      if (waited == pid) {
        child_running = false;
      }
    }

    fd_set rfds;
    FD_ZERO(&rfds);
    int maxfd = -1;
    if (stdout_open) {
      FD_SET(stdout_pipe[0], &rfds);
      if (stdout_pipe[0] > maxfd) maxfd = stdout_pipe[0];
    }
    if (stderr_open) {
      FD_SET(stderr_pipe[0], &rfds);
      if (stderr_pipe[0] > maxfd) maxfd = stderr_pipe[0];
    }

    struct timeval tv{};
    struct timeval* tv_ptr = nullptr;
    if (options.timeout_ms > 0) {
      const auto now = std::chrono::steady_clock::now();
      const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - start_time).count();
      if (elapsed_ms >= options.timeout_ms) {
        if (child_running) {
          kill(pid, options.kill_signal);
          (void)waitpid(pid, &status, 0);
          child_running = false;
          timed_out = true;
        }
      } else {
        tv.tv_sec = 0;
        tv.tv_usec = 100000;
        tv_ptr = &tv;
      }
    } else if (child_running) {
      tv.tv_sec = 0;
      tv.tv_usec = 100000;
      tv_ptr = &tv;
    }

    if (maxfd >= 0) {
      const int selected = select(maxfd + 1, &rfds, nullptr, nullptr, tv_ptr);
      if (selected > 0) {
        if (stdout_open && FD_ISSET(stdout_pipe[0], &rfds)) {
          AppendFromFd(stdout_pipe[0], &out_stdout, &stdout_open);
        }
        if (stderr_open && FD_ISSET(stderr_pipe[0], &rfds)) {
          AppendFromFd(stderr_pipe[0], &out_stderr, &stderr_open);
        }
      } else if (selected == 0) {
        if (stdout_open) AppendFromFd(stdout_pipe[0], &out_stdout, &stdout_open);
        if (stderr_open) AppendFromFd(stderr_pipe[0], &out_stderr, &stderr_open);
      }
      if (options.max_buffer >= 0 &&
          (static_cast<int64_t>(out_stdout.size()) > options.max_buffer ||
           static_cast<int64_t>(out_stderr.size()) > options.max_buffer)) {
        max_buffer_exceeded = true;
        if (child_running) {
          kill(pid, options.kill_signal);
          (void)waitpid(pid, &status, 0);
          child_running = false;
        }
      }
    } else if (!child_running) {
      break;
    }
  }

  int err_no = 0;
  const ssize_t err_read = read(exec_err_pipe[0], &err_no, sizeof(err_no));
  if (err_read == static_cast<ssize_t>(sizeof(err_no))) {
    spawn_errno = err_no;
  }
  close(exec_err_pipe[0]);

  napi_value result = nullptr;
  napi_create_object(env, &result);

  napi_value pid_value = nullptr;
  napi_create_int32(env, static_cast<int32_t>(pid), &pid_value);
  napi_set_named_property(env, result, "pid", pid_value);

  napi_value output = nullptr;
  napi_create_array_with_length(env, 3, &output);
  napi_value null_value = nullptr;
  napi_get_null(env, &null_value);
  napi_set_element(env, output, 0, null_value);

  napi_value stdout_val = nullptr;
  const char* stdout_ptr = out_stdout.empty() ? "" : reinterpret_cast<const char*>(out_stdout.data());
  napi_create_string_utf8(env, stdout_ptr, out_stdout.size(), &stdout_val);
  napi_set_element(env, output, 1, stdout_val);

  napi_value stderr_val = nullptr;
  const char* stderr_ptr = out_stderr.empty() ? "" : reinterpret_cast<const char*>(out_stderr.data());
  napi_create_string_utf8(env, stderr_ptr, out_stderr.size(), &stderr_val);
  napi_set_element(env, output, 2, stderr_val);
  napi_set_named_property(env, result, "output", output);

  if (spawn_errno != 0) {
    napi_value error_value = nullptr;
    napi_create_int32(env, -spawn_errno, &error_value);
    napi_set_named_property(env, result, "error", error_value);
    napi_set_named_property(env, result, "status", null_value);
    napi_set_named_property(env, result, "signal", null_value);
    return result;
  }

  if (WIFEXITED(status)) {
    napi_value status_value = nullptr;
    napi_create_int32(env, WEXITSTATUS(status), &status_value);
    napi_set_named_property(env, result, "status", status_value);
    napi_set_named_property(env, result, "signal", null_value);
  } else if (WIFSIGNALED(status)) {
    napi_set_named_property(env, result, "status", null_value);
    const char* sig_name = SignalName(WTERMSIG(status));
    if (sig_name != nullptr) {
      napi_value signal_value = nullptr;
      napi_create_string_utf8(env, sig_name, NAPI_AUTO_LENGTH, &signal_value);
      napi_set_named_property(env, result, "signal", signal_value);
    } else {
      napi_set_named_property(env, result, "signal", null_value);
    }
  } else {
    napi_set_named_property(env, result, "status", null_value);
    napi_set_named_property(env, result, "signal", null_value);
  }

  if (timed_out) {
    napi_value error_value = nullptr;
    napi_create_int32(env, -ETIMEDOUT, &error_value);
    napi_set_named_property(env, result, "error", error_value);
  } else if (max_buffer_exceeded) {
    napi_value error_value = nullptr;
    napi_create_int32(env, -ENOBUFS, &error_value);
    napi_set_named_property(env, result, "error", error_value);
  }
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

void UnodeInstallSpawnSyncBinding(napi_env env) {
  if (env == nullptr) return;
  napi_value binding = nullptr;
  if (napi_create_object(env, &binding) != napi_ok || binding == nullptr) return;
  SetMethod(env, binding, "spawn", SpawnSync);

  napi_value global = nullptr;
  if (napi_get_global(env, &global) != napi_ok || global == nullptr) return;
  napi_set_named_property(env, global, "__unode_spawn_sync", binding);
}
