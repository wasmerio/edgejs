#include "ubi_cli.h"

#include <csignal>
#include <cstring>
#include <filesystem>
#include <functional>
#include <iostream>
#include <mutex>
#include <string>
#include <vector>

#include "node_version.h"
#include "unofficial_napi.h"
#include "ubi_process.h"
#include "ubi_runtime.h"

namespace {

constexpr const char kUsage[] = "Usage: ubi <script.js>";
constexpr unsigned kMaxSignal = 32;
std::once_flag g_cli_init_once;

void ResetSignalHandlersLikeNode() {
#if defined(__POSIX__)
  struct sigaction act;
  std::memset(&act, 0, sizeof(act));

  for (unsigned nr = 1; nr < kMaxSignal; nr += 1) {
    if (nr == SIGKILL || nr == SIGSTOP) continue;

    bool ignore_signal = false;
#if defined(SIGPIPE)
    ignore_signal = ignore_signal || nr == SIGPIPE;
#endif
#if defined(SIGXFSZ)
    ignore_signal = ignore_signal || nr == SIGXFSZ;
#endif
    act.sa_handler = ignore_signal ? SIG_IGN : SIG_DFL;

    if (act.sa_handler == SIG_DFL) {
      struct sigaction old;
      if (sigaction(static_cast<int>(nr), nullptr, &old) != 0) continue;
#if defined(SA_SIGINFO)
      if ((old.sa_flags & SA_SIGINFO) || old.sa_handler != SIG_IGN) continue;
#else
      if (old.sa_handler != SIG_IGN) continue;
#endif
    }

    (void)sigaction(static_cast<int>(nr), &act, nullptr);
  }
#endif
}

int RunWithFreshEnv(const std::function<int(napi_env)>& runner, std::string* error_out) {
  napi_env env = nullptr;
  void* env_scope = nullptr;
  const napi_status create_status = unofficial_napi_create_env(8, &env, &env_scope);
  if (create_status != napi_ok || env == nullptr || env_scope == nullptr) {
    if (error_out != nullptr) {
      *error_out = "Failed to initialize runtime environment";
    }
    return 1;
  }

  const int exit_code = runner(env);
  const napi_status release_status = unofficial_napi_release_env(env_scope);
  if (release_status != napi_ok) {
    if (error_out != nullptr) {
      *error_out = "Failed to release runtime environment";
    }
    return 1;
  }
  return exit_code;
}

std::string ResolveCliScriptPath(const char* script_path) {
  if (script_path == nullptr || script_path[0] == '\0') {
    return "";
  }
  std::filesystem::path direct(script_path);
  if (direct.is_absolute() || std::filesystem::exists(direct)) {
    return direct.string();
  }

  // Allow running `./build/ubi examples/foo.js` from repo root.
  const std::filesystem::path repo_fallback = std::filesystem::path("ubi") / direct;
  if (std::filesystem::exists(repo_fallback)) {
    return repo_fallback.string();
  }
  return direct.string();
}

std::vector<std::string> NormalizeCliOptionVector(const std::vector<std::string>& raw_args) {
  std::vector<std::string> out;
  out.reserve(raw_args.size());

  for (size_t i = 0; i < raw_args.size(); ++i) {
    const std::string& token = raw_args[i];

    if ((token == "-e" || token == "--eval") && i + 1 < raw_args.size()) {
      out.push_back("--eval=" + raw_args[++i]);
      continue;
    }

    if (token == "-p" || token == "--print") {
      out.push_back("--print");
      if (i + 1 < raw_args.size()) {
        out.push_back("--eval=" + raw_args[++i]);
      }
      continue;
    }

    if ((token == "-pe" || token == "-ep") && i + 1 < raw_args.size()) {
      out.push_back("--print");
      out.push_back("--eval=" + raw_args[++i]);
      continue;
    }

    out.push_back(token);
  }

  return out;
}

}  // namespace

void UbiInitializeCliProcess() {
  std::call_once(g_cli_init_once, []() {
    ResetSignalHandlersLikeNode();
  });
}

int UbiRunCliScript(const char* script_path, std::string* error_out) {
  UbiInitializeCliProcess();
  if (error_out != nullptr) {
    error_out->clear();
  }
  if (script_path == nullptr || script_path[0] == '\0') {
    if (error_out != nullptr) {
      *error_out = kUsage;
    }
    return 1;
  }

  const std::string resolved_script_path = ResolveCliScriptPath(script_path);
  return RunWithFreshEnv(
      [&](napi_env env) {
        return UbiRunScriptFileWithLoop(env, resolved_script_path.c_str(), error_out, true);
      },
      error_out);
}

int UbiRunCli(int argc, const char* const* argv, std::string* error_out) {
  UbiInitializeCliProcess();
  if (error_out != nullptr) {
    error_out->clear();
  }
  if (argv != nullptr && argc > 0 && argv[0] != nullptr) {
    UbiSetProcessArgv0(argv[0]);
  }
  if (argv == nullptr || argc < 2) {
    UbiSetExecArgv({});
    UbiSetScriptArgv({});
    if (error_out != nullptr) {
      *error_out = kUsage;
    }
    return 1;
  }
  if (argv[1] != nullptr &&
      (std::string(argv[1]) == "-v" || std::string(argv[1]) == "--version")) {
    std::cout << NODE_VERSION << "\n";
    return 0;
  }
  int mode_index = -1;
  std::string mode;
  for (int i = 1; i < argc; ++i) {
    if (argv[i] == nullptr) continue;
    const std::string token(argv[i]);
    if (token == "-e" || token == "--eval" || token == "-p" || token == "--print" ||
        token == "-pe" || token == "-ep" || token == "-i" || token == "--interactive") {
      mode_index = i;
      mode = token;
      break;
    }
  }

  const bool is_interactive = (mode == "-i" || mode == "--interactive");
  if (is_interactive) {
    for (int i = mode_index + 1; i < argc; ++i) {
      if (argv[i] == nullptr) continue;
      const std::string token(argv[i]);
      if (token == "--input-type" || token.rfind("--input-type=", 0) == 0) {
        if (error_out != nullptr) {
          *error_out = "Cannot specify --input-type for REPL";
        }
        return 1;
      }
    }
    std::vector<std::string> raw_exec_argv;
    raw_exec_argv.reserve(static_cast<size_t>(argc - 1));
    for (int i = 1; i < argc; ++i) {
      if (argv[i] != nullptr) raw_exec_argv.emplace_back(argv[i]);
    }
    UbiSetExecArgv(NormalizeCliOptionVector(raw_exec_argv));
    UbiSetScriptArgv({});
    static constexpr const char kInteractiveMain[] = "require('internal/main/repl');";
    return RunWithFreshEnv(
        [&](napi_env env) { return UbiRunScriptSourceWithLoop(env, kInteractiveMain, error_out, true); },
        error_out);
  }

  const bool is_eval_mode = (mode == "-e" || mode == "--eval" || mode == "-pe" || mode == "-ep");
  const bool is_print_mode = (mode == "-p" || mode == "--print" || mode == "-pe" || mode == "-ep");
  if (is_eval_mode || is_print_mode) {
    if (mode_index < 0 || mode_index + 1 >= argc || argv[mode_index + 1] == nullptr) {
      if (error_out != nullptr) {
        *error_out = kUsage;
      }
      return 1;
    }
    std::vector<std::string> script_argv;
    script_argv.reserve(static_cast<size_t>(argc - (mode_index + 2)));
    for (int i = mode_index + 2; i < argc; ++i) {
      if (argv[i] != nullptr) {
        script_argv.emplace_back(argv[i]);
      }
    }
    std::vector<std::string> raw_exec_argv;
    raw_exec_argv.reserve(static_cast<size_t>(mode_index + 1));
    for (int i = 1; i <= mode_index + 1 && i < argc; ++i) {
      if (argv[i] != nullptr) raw_exec_argv.emplace_back(argv[i]);
    }
    UbiSetExecArgv(NormalizeCliOptionVector(raw_exec_argv));
    UbiSetScriptArgv(script_argv);
    static constexpr const char kEvalStringMain[] = "require('internal/main/eval_string');";
    return RunWithFreshEnv(
        [&](napi_env env) {
          return UbiRunScriptSourceWithLoop(env, kEvalStringMain, error_out, true);
        },
        error_out);
  }

  int script_index = 1;
  std::vector<std::string> exec_argv;
  exec_argv.reserve(static_cast<size_t>(argc));
  while (script_index < argc && argv[script_index] != nullptr) {
    const std::string token(argv[script_index]);
    if (token == "--") {
      script_index++;
      break;
    }
    if (!token.empty() && token[0] == '-') {
      exec_argv.push_back(token);
      script_index++;
      continue;
    }
    break;
  }
  UbiSetExecArgv(exec_argv);
  if (script_index >= argc || argv[script_index] == nullptr) {
    UbiSetScriptArgv({});
    if (error_out != nullptr) {
      *error_out = kUsage;
    }
    return 1;
  }

  std::vector<std::string> script_argv;
  script_argv.reserve(static_cast<size_t>(argc - (script_index + 1)));
  for (int i = script_index + 1; i < argc; ++i) {
    if (argv[i] != nullptr) {
      script_argv.emplace_back(argv[i]);
    }
  }
  UbiSetScriptArgv(script_argv);
  return UbiRunCliScript(argv[script_index], error_out);
}
