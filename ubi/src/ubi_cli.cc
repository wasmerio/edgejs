#include "ubi_cli.h"

#include <csignal>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#if defined(_WIN32)
#include <io.h>
#else
#include <unistd.h>
#endif

#include <uv.h>

#include "node_version.h"
#include "unofficial_napi.h"
#include "ubi_option_helpers.h"
#include "ubi_compat_exec.h"
#include "ubi_process.h"
#include "ubi_runtime_platform.h"
#include "ubi_runtime.h"

namespace {

constexpr const char kUsage[] = "Usage: ubi <script.js>";
constexpr const char kSafeModeUnavailable[] = "--safe mode is not enabled in this release of ubi";
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
  if (!UbiInitializeOpenSslForCli(error_out)) {
    return 1;
  }

  napi_env env = nullptr;
  void* env_scope = nullptr;
  const napi_status create_status = unofficial_napi_create_env(8, &env, &env_scope);
  if (create_status != napi_ok || env == nullptr || env_scope == nullptr) {
    if (error_out != nullptr) {
      *error_out = "Failed to initialize runtime environment";
    }
    return 1;
  }

  if (UbiRuntimePlatformInstallHooks(env) != napi_ok) {
    (void)unofficial_napi_release_env(env_scope);
    if (error_out != nullptr) {
      *error_out = "Failed to attach runtime platform hooks";
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
  auto resolve_candidate = [](const std::filesystem::path& candidate) -> std::string {
    static constexpr const char* kExtensions[] = {
        "", ".js", ".json", ".node", "/index.js", "/index.json", "/index.node",
    };

    std::error_code ec;
    for (const char* suffix : kExtensions) {
      const std::filesystem::path resolved = candidate.string() + suffix;
      if (std::filesystem::exists(resolved, ec) && !ec) {
        return resolved.string();
      }
      ec.clear();
    }
    return "";
  };

  const std::filesystem::path direct(script_path);
  if (const std::string resolved = resolve_candidate(direct); !resolved.empty()) {
    return resolved;
  }

  // Allow running `./build/ubi examples/foo.js` from repo root.
  const std::filesystem::path repo_fallback = std::filesystem::path("ubi") / direct;
  if (const std::string resolved = resolve_candidate(repo_fallback); !resolved.empty()) {
    return resolved;
  }
  return direct.string();
}

std::string CliErrorPrefix() {
  std::string exec_path = UbiGetProcessExecPath();
  if (exec_path.empty()) exec_path = "ubi";
  return exec_path;
}

std::string FormatCliError(const std::string& message) {
  return CliErrorPrefix() + ": " + message;
}

bool ApplyEnvUpdate(const std::string& key, const std::string& value) {
#if defined(_WIN32)
  return _putenv_s(key.c_str(), value.c_str()) == 0;
#else
  return setenv(key.c_str(), value.c_str(), 1) == 0;
#endif
}

void ApplyEnvUpdates(const std::unordered_map<std::string, std::string>& updates) {
  for (const auto& [key, value] : updates) {
    (void)ApplyEnvUpdate(key, value);
  }
}

bool TokenHasInlineValue(const std::string& token) {
  return token.find('=') != std::string::npos;
}

bool OptionConsumesNextToken(const std::string& token) {
  static const std::unordered_set<std::string> kValueOptions = {
      "-e",
      "--eval",
      "-pe",
      "-ep",
      "-r",
      "--require",
      "--input-type",
      "--inspect-port",
      "--debug-port",
      "--stack-trace-limit",
      "--secure-heap",
      "--secure-heap-min",
      "--disable-warning",
      "--env-file",
      "--env-file-if-exists",
      "--experimental-config-file",
      "--experimental-loader",
      "--loader",
      "--import",
      "--conditions",
      "--run",
      "--allow-fs-read",
      "--allow-fs-write",
  };
  return kValueOptions.find(token) != kValueOptions.end();
}

bool TokenRequiresNonEmptyInlineValue(const std::string& token) {
  static const std::unordered_set<std::string> kRequireValue = {
      "--debug-port=",
      "--inspect-port=",
  };
  for (const auto& prefix : kRequireValue) {
    if (token == prefix) return true;
  }
  return false;
}

bool IsBooleanOptionForNegation(const std::string& option) {
  static const std::unordered_set<std::string> kBooleanOptions = {
      "--abort-on-uncaught-exception",
      "--allow-addons",
      "--allow-child-process",
      "--allow-inspector",
      "--allow-wasi",
      "--allow-worker",
      "--async-context-frame",
      "--check",
      "--enable-source-maps",
      "--entry-url",
      "--experimental-addon-modules",
      "--experimental-detect-module",
      "--experimental-eventsource",
      "--experimental-fetch",
      "--experimental-global-customevent",
      "--experimental-global-webcrypto",
      "--experimental-import-meta-resolve",
      "--experimental-inspector-network-resource",
      "--experimental-network-inspection",
      "--experimental-print-required-tla",
      "--experimental-quic",
      "--experimental-require-module",
      "--experimental-report",
      "--experimental-sqlite",
      "--experimental-test-coverage",
      "--experimental-test-module-mocks",
      "--experimental-transform-types",
      "--experimental-vm-modules",
      "--experimental-wasm-modules",
      "--experimental-webstorage",
      "--experimental-worker",
      "--expose-internals",
      "--force-fips",
      "--frozen-intrinsics",
      "--insecure-http-parser",
      "--inspect-brk",
      "--interactive",
      "--network-family-autoselection",
      "--no-addons",
      "--no-deprecation",
      "--no-experimental-global-navigator",
      "--no-experimental-websocket",
      "--no-node-snapshot",
      "--no-verify-base-objects",
      "--node-snapshot",
      "--openssl-legacy-provider",
      "--openssl-shared-config",
      "--pending-deprecation",
      "--permission",
      "--preserve-symlinks",
      "--preserve-symlinks-main",
      "--print",
      "--report-on-signal",
      "--strip-types",
      "--test",
      "--test-force-exit",
      "--test-only",
      "--test-update-snapshots",
      "--throw-deprecation",
      "--tls-max-v1.2",
      "--tls-max-v1.3",
      "--tls-min-v1.0",
      "--tls-min-v1.1",
      "--tls-min-v1.2",
      "--tls-min-v1.3",
      "--trace-deprecation",
      "--trace-require-module",
      "--trace-sigint",
      "--trace-tls",
      "--trace-warnings",
      "--use-bundled-ca",
      "--use-env-proxy",
      "--use-openssl-ca",
      "--use-system-ca",
      "--verify-base-objects",
      "--warnings",
      "--watch",
      "--watch-preserve-output",
  };
  return kBooleanOptions.find(option) != kBooleanOptions.end();
}

bool IsKnownNonBooleanOption(const std::string& option) {
  static const std::unordered_set<std::string> kNonBooleanOptions = {
      "--allow-fs-read",
      "--allow-fs-write",
      "--debug-port",
      "--diagnostic-dir",
      "--disable-warning",
      "--dns-result-order",
      "--env-file",
      "--env-file-if-exists",
      "--es-module-specifier-resolution",
      "--eval",
      "--experimental-config-file",
      "--heapsnapshot-signal",
      "--icu-data-dir",
      "--input-type",
      "--inspect-port",
      "--localstorage-file",
      "--max-http-header-size",
      "--openssl-config",
      "--redirect-warnings",
      "--require",
      "--run",
      "--secure-heap",
      "--secure-heap-min",
      "--stack-trace-limit",
      "--test-global-setup",
      "--test-isolation",
      "--test-rerun-failures",
      "--test-shard",
      "--tls-cipher-list",
      "--tls-keylog",
      "--unhandled-rejections",
      "--watch-kill-signal",
  };
  return kNonBooleanOptions.find(option) != kNonBooleanOptions.end();
}

bool ValidateNegatedOption(const std::string& token, std::string* error_out) {
  if (token.rfind("--no-", 0) != 0) return true;
  if (IsBooleanOptionForNegation(token)) return true;
  const std::string normalized = "--" + token.substr(5);
  if (IsBooleanOptionForNegation(normalized)) return true;
  if (IsKnownNonBooleanOption(normalized)) {
    if (error_out != nullptr) {
      *error_out = FormatCliError(token + " is an invalid negation because it is not a boolean option");
    }
    return false;
  }
  if (error_out != nullptr) {
    *error_out = "bad option: " + token;
  }
  return false;
}

bool HasDisallowedNodeOption(const std::string& token) {
  static const std::unordered_set<std::string> kDisallowedExact = {
      "--",
      "--check",
      "--eval",
      "--expose-internals",
      "--expose_internals",
      "--help",
      "--interactive",
      "--print",
      "--test",
      "--v8-options",
      "--version",
      "-c",
      "-e",
      "-h",
      "-i",
      "-p",
      "-pe",
      "-v",
  };
  const size_t eq = token.find('=');
  const std::string key = eq == std::string::npos ? token : token.substr(0, eq);
  return kDisallowedExact.find(key) != kDisallowedExact.end();
}

bool IsRecognizedCliOptionToken(const std::string& token) {
  if (token == "--" || token == "-") return true;
  if (token == "-c" || token == "--check" ||
      token == "-e" || token == "--eval" ||
      token == "-i" || token == "--interactive" ||
      token == "-p" || token == "--print" ||
      token == "-pe" || token == "-ep" ||
      token == "-r" || token == "--require" ||
      token == "--run") {
    return true;
  }
  if (TokenHasInlineValue(token)) {
    const std::string key = token.substr(0, token.find('='));
    return OptionConsumesNextToken(key) ||
           key == "--env-file" ||
           key == "--env-file-if-exists" ||
           key == "--experimental-config-file" ||
           key == "--input-type";
  }
  if (OptionConsumesNextToken(token)) return true;
  if (IsBooleanOptionForNegation(token)) return true;
  if (token.rfind("--no-", 0) == 0) return true;
  if (token.rfind("--env-file=", 0) == 0 ||
      token.rfind("--env-file-if-exists=", 0) == 0 ||
      token.rfind("--experimental-config-file=", 0) == 0 ||
      token.rfind("--input-type=", 0) == 0) {
    return true;
  }
  return false;
}

bool ValidateNodeOptions(const std::vector<std::string>& node_options_tokens, std::string* error_out) {
  for (const auto& token : node_options_tokens) {
    if (HasDisallowedNodeOption(token)) {
      if (error_out != nullptr) {
        *error_out = FormatCliError(token + " is not allowed in NODE_OPTIONS");
      }
      return false;
    }
  }
  return true;
}

bool HasExactOptionToken(const std::vector<std::string>& tokens, const char* option) {
  for (const auto& token : tokens) {
    if (token == option) return true;
  }
  return false;
}

bool ValidateCaOptions(const ubi_options::EffectiveCliState& state, std::string* error_out) {
  const bool use_openssl_ca = HasExactOptionToken(state.effective_tokens, "--use-openssl-ca");
  const bool use_bundled_ca = HasExactOptionToken(state.effective_tokens, "--use-bundled-ca");
  if (use_openssl_ca && use_bundled_ca) {
    if (error_out != nullptr) {
      *error_out = FormatCliError("either --use-openssl-ca or --use-bundled-ca can be used, not both");
    }
    return false;
  }
  return true;
}

bool RawExecArgvHasInputType(const std::vector<std::string>& raw_exec_argv) {
  for (const auto& token : raw_exec_argv) {
    if (token == "--input-type" || token.rfind("--input-type=", 0) == 0) {
      return true;
    }
  }
  return false;
}

bool IsPermissionFlagToken(const std::string& token) {
  const size_t eq = token.find('=');
  const std::string key = eq == std::string::npos ? token : token.substr(0, eq);
  return key == "--permission" ||
         key == "--allow-fs-read" ||
         key == "--allow-fs-write" ||
         key == "--allow-addons" ||
         key == "--allow-child-process" ||
         key == "--allow-inspector" ||
         key == "--allow-worker" ||
         key == "--allow-wasi";
}

bool AreProcessWarningsSuppressed() {
  const char* value = std::getenv("NODE_NO_WARNINGS");
  if (value == nullptr || value[0] == '\0') return false;
  return std::strcmp(value, "0") != 0;
}

void WarnUnsupportedPermissionsIfNeeded(const ubi_options::EffectiveCliState& state) {
  if (AreProcessWarningsSuppressed()) return;
  for (const auto& token : state.effective_tokens) {
    if (!IsPermissionFlagToken(token)) continue;
    std::cerr << "Warning: permissions are not supported in Ubi; ignoring permission flags.\n";
    return;
  }
}

bool TryLoadPackageScripts(const std::filesystem::path& package_json_path,
                           std::vector<std::pair<std::string, std::string>>* scripts_out) {
  if (scripts_out == nullptr) return false;
  scripts_out->clear();

  std::ifstream input(package_json_path, std::ios::in | std::ios::binary);
  if (!input.is_open()) return false;
  std::ostringstream buffer;
  buffer << input.rdbuf();

  simdjson::ondemand::parser parser;
  simdjson::padded_string padded(buffer.str());
  simdjson::ondemand::document document;
  simdjson::ondemand::object root;
  if (parser.iterate(padded).get(document) != simdjson::SUCCESS ||
      document.get_object().get(root) != simdjson::SUCCESS) {
    return false;
  }

  simdjson::ondemand::value scripts_value;
  if (root["scripts"].get(scripts_value) != simdjson::SUCCESS) return false;
  simdjson::ondemand::object scripts;
  if (scripts_value.get_object().get(scripts) != simdjson::SUCCESS) return false;

  for (auto field : scripts) {
    std::string_view raw_key;
    std::string_view value;
    if (field.unescaped_key().get(raw_key) != simdjson::SUCCESS ||
        field.value().get_string().get(value) != simdjson::SUCCESS) {
      continue;
    }
    scripts_out->push_back({std::string(raw_key), std::string(value)});
  }
  return true;
}

int RunCliPackageScript(const std::string& script_name, std::string* error_out) {
  const std::filesystem::path package_json_path =
      std::filesystem::current_path() / "package.json";
  std::vector<std::pair<std::string, std::string>> scripts;
  const bool has_scripts = TryLoadPackageScripts(package_json_path, &scripts);

  if (!has_scripts) {
    if (error_out != nullptr) {
      *error_out = "Missing script: \"" + script_name + "\"";
    }
    return 1;
  }

  for (const auto& [name, _] : scripts) {
    if (name == script_name) {
      if (error_out != nullptr) {
        *error_out = "The --run launcher is not implemented for existing scripts yet";
      }
      return 1;
    }
  }

  if (error_out != nullptr) {
    *error_out = "Missing script: \"" + script_name + "\" for " +
                 package_json_path.string() + "\n\nAvailable scripts are:\n";
    for (const auto& [name, value] : scripts) {
      *error_out += "  " + name + ": " + value + "\n";
    }
  }
  return 1;
}

bool StdinIsTTY() {
#if defined(_WIN32)
  return uv_guess_handle(_fileno(stdin)) == UV_TTY;
#else
  return uv_guess_handle(STDIN_FILENO) == UV_TTY;
#endif
}

int RunCliBuiltin(const char* source_text, std::string* error_out) {
  return RunWithFreshEnv(
      [&](napi_env env) { return UbiRunScriptSourceWithLoop(env, source_text, error_out, true); },
      error_out);
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
  if (argv == nullptr || argc < 1) {
    if (error_out != nullptr) *error_out = kUsage;
    return 1;
  }
  if (argc > 1 && argv[1] != nullptr && UbiShouldWrapCompatCommand(argv[1])) {
    return UbiRunCompatCommand(argc, argv, error_out);
  }
  if (argc > 1 && argv[1] != nullptr &&
      (std::string(argv[1]) == "-v" || std::string(argv[1]) == "--version")) {
    std::cout << NODE_VERSION << "\n";
    return 0;
  }
  enum class CliMode {
    kNone,
    kInteractive,
    kEval,
    kPrint,
    kCheck,
    kRun,
  };

  CliMode mode = CliMode::kNone;
  std::vector<std::string> raw_exec_argv;
  std::vector<std::string> script_argv;
  raw_exec_argv.reserve(static_cast<size_t>(argc));
  int script_index = argc;
  std::string run_target;
  bool saw_check = false;
  bool print_flag = false;
  bool has_eval_string = false;
  bool force_repl = false;

  auto set_requires_argument_error = [&](const std::string& token) {
    if (error_out != nullptr) {
      *error_out = FormatCliError(token + " requires an argument");
    }
  };

  auto finalize_effective_state = [&](ubi_options::EffectiveCliState* out_state) -> bool {
    if (out_state == nullptr) return false;
    *out_state = ubi_options::BuildEffectiveCliState(raw_exec_argv);
    if (!out_state->ok) {
      if (error_out != nullptr) {
        *error_out = FormatCliError(out_state->error);
      }
      return false;
    }
    if (!ValidateNodeOptions(out_state->node_options_tokens, error_out)) {
      return false;
    }
    if (!ValidateCaOptions(*out_state, error_out)) {
      return false;
    }
    ApplyEnvUpdates(out_state->env_updates);
    for (const auto& warning : out_state->warnings) {
      std::cerr << warning << "\n";
    }
    WarnUnsupportedPermissionsIfNeeded(*out_state);
    return true;
  };

  int i = 1;
  for (; i < argc; ++i) {
    if (argv[i] == nullptr) continue;
    const std::string token(argv[i]);

    if (token == "--") {
      script_index = i + 1;
      break;
    }
    if (token == "-") {
      script_index = i;
      break;
    }
    if (token.empty() || token[0] != '-') {
      script_index = i;
      break;
    }
    if (token.rfind("--env-file-", 0) == 0 &&
        token != "--env-file" &&
        token.rfind("--env-file=", 0) != 0 &&
        token != "--env-file-if-exists" &&
        token.rfind("--env-file-if-exists=", 0) != 0) {
      if (error_out != nullptr) {
        *error_out = "bad option: " + token;
      }
      return 9;
    }
    if (!ValidateNegatedOption(token, error_out)) {
      return 9;
    }
    if (TokenRequiresNonEmptyInlineValue(token)) {
      set_requires_argument_error(token.substr(0, token.size() - 1));
      return 9;
    }
    if (token == "--safe") {
      if (error_out != nullptr) {
        *error_out = kSafeModeUnavailable;
      }
      return 1;
    }

    if (token == "-c" || token == "--check") {
      raw_exec_argv.push_back(token);
      saw_check = true;
      mode = CliMode::kCheck;
      continue;
    }
    if (token == "-i" || token == "--interactive") {
      raw_exec_argv.push_back(token);
      force_repl = true;
      mode = CliMode::kInteractive;
      continue;
    }
    if (token == "-e" || token == "--eval" ||
        token == "-pe" || token == "-ep") {
      if (saw_check) {
        if (error_out != nullptr) {
          *error_out = FormatCliError("either --check or --eval can be used, not both");
        }
        return 9;
      }
      if (i + 1 >= argc || argv[i + 1] == nullptr) {
        set_requires_argument_error(token);
        return 9;
      }
      raw_exec_argv.push_back(token);
      raw_exec_argv.emplace_back(argv[++i]);
      has_eval_string = true;
      if (token == "-p" || token == "--print" || token == "-pe" || token == "-ep") {
        print_flag = true;
      }
      mode = print_flag ? CliMode::kPrint : CliMode::kEval;
      script_index = i + 1;
      break;
    }
    if (token == "-p" || token == "--print") {
      raw_exec_argv.push_back(token);
      print_flag = true;
      mode = CliMode::kPrint;
      if (i + 1 < argc && argv[i + 1] != nullptr) {
        const std::string next(argv[i + 1]);
        if (!IsRecognizedCliOptionToken(next)) {
          raw_exec_argv.emplace_back(argv[++i]);
          has_eval_string = true;
          script_index = i + 1;
          break;
        }
      }
      continue;
    }
    if (token == "--run") {
      if (i + 1 >= argc || argv[i + 1] == nullptr) {
        set_requires_argument_error(token);
        return 9;
      }
      raw_exec_argv.push_back(token);
      run_target = argv[++i];
      raw_exec_argv.push_back(run_target);
      mode = CliMode::kRun;
      script_index = i + 1;
      break;
    }

    raw_exec_argv.push_back(token);
    if (TokenHasInlineValue(token)) continue;
    if (OptionConsumesNextToken(token)) {
      if (i + 1 >= argc || argv[i + 1] == nullptr) {
        set_requires_argument_error(token);
        return 9;
      }
      raw_exec_argv.emplace_back(argv[++i]);
    }
  }

  if (script_index == argc) script_index = i;

  ubi_options::EffectiveCliState effective_state;
  if (!finalize_effective_state(&effective_state)) {
    return 9;
  }

  UbiSetExecArgv(raw_exec_argv);

  if (force_repl) {
    if (RawExecArgvHasInputType(raw_exec_argv)) {
      if (error_out != nullptr) {
        *error_out = "Cannot specify --input-type for REPL";
      }
      return 1;
    }
    UbiSetScriptArgv({});
    static constexpr const char kInteractiveMain[] = "require('internal/main/repl');";
    return RunCliBuiltin(kInteractiveMain, error_out);
  }

  if (has_eval_string || (print_flag && mode == CliMode::kPrint)) {
    if (script_index < argc && argv[script_index] != nullptr && std::string(argv[script_index]) == "--") {
      script_index++;
    }
    script_argv.reserve(static_cast<size_t>(argc - script_index));
    for (int argi = script_index; argi < argc; ++argi) {
      if (argv[argi] != nullptr) script_argv.emplace_back(argv[argi]);
    }
    UbiSetScriptArgv(script_argv);
    static constexpr const char kEvalStringMain[] = "require('internal/main/eval_string');";
    return RunCliBuiltin(kEvalStringMain, error_out);
  }

  if (mode == CliMode::kRun) {
    return RunCliPackageScript(run_target, error_out);
  }

  const bool use_stdin_entry =
      script_index >= argc || argv[script_index] == nullptr || std::string(argv[script_index]) == "-";
  if (use_stdin_entry) {
    if (script_index < argc && argv[script_index] != nullptr && std::string(argv[script_index]) == "-") {
      script_argv.reserve(static_cast<size_t>(argc - (script_index + 1)));
      for (int argi = script_index + 1; argi < argc; ++argi) {
        if (argv[argi] != nullptr) script_argv.emplace_back(argv[argi]);
      }
    }
    UbiSetScriptArgv(script_argv);
    static constexpr const char kInteractiveMain[] = "require('internal/main/repl');";
    static constexpr const char kEvalStdinMain[] = "require('internal/main/eval_stdin');";
    static constexpr const char kCheckSyntaxMain[] = "require('internal/main/check_syntax');";
    static constexpr const char kCheckSyntaxModuleStdinMain[] =
        "/*__ubi_skip_pre_execution__*/"
        "const { prepareMainThreadExecution, markBootstrapComplete } = "
        "require('internal/process/pre_execution');"
        "const { readStdin } = require('internal/process/execution');"
        "prepareMainThreadExecution(true);"
        "markBootstrapComplete();"
        "readStdin((code) => {"
        "  try {"
        "    const { ModuleWrap } = internalBinding('module_wrap');"
        "    new ModuleWrap('[stdin]', undefined, code, 0, 0);"
        "  } catch (err) {"
        "    if (err && typeof err.stack === 'string' && !err.stack.startsWith('[stdin]')) {"
        "      err.stack = '[stdin]\\n' + err.stack;"
        "    }"
        "    throw err;"
        "  }"
        "});";
    if (mode == CliMode::kCheck) {
      if (RawExecArgvHasInputType(raw_exec_argv)) {
        return RunCliBuiltin(kCheckSyntaxModuleStdinMain, error_out);
      }
      return RunCliBuiltin(kCheckSyntaxMain, error_out);
    }
    return RunCliBuiltin(StdinIsTTY() ? kInteractiveMain : kEvalStdinMain, error_out);
  }

  script_argv.reserve(static_cast<size_t>(argc - (script_index + 1)));
  for (int argi = script_index + 1; argi < argc; ++argi) {
    if (argv[argi] != nullptr) script_argv.emplace_back(argv[argi]);
  }
  UbiSetScriptArgv(script_argv);
  return UbiRunCliScript(argv[script_index], error_out);
}

int UbiRunEnvCli(int argc, const char* const* argv, std::string* error_out) {
  UbiInitializeCliProcess();
  if (error_out != nullptr) {
    error_out->clear();
  }
  if (argv != nullptr && argc > 0 && argv[0] != nullptr) {
    UbiSetProcessArgv0(argv[0]);
  }
  return UbiRunCompatCommand(argc, argv, error_out);
}
