#include "unode_cli.h"

#include <filesystem>
#include <functional>
#include <iostream>
#include <string>
#include <vector>

#include "../../node/src/node_version.h"
#include "unofficial_napi.h"
#include "unode_runtime.h"

namespace {

constexpr const char kUsage[] = "Usage: unode <script.js>";

std::string JsonQuote(const std::string& text) {
  std::string out;
  out.reserve(text.size() + 2);
  out.push_back('"');
  for (char ch : text) {
    switch (ch) {
      case '\\':
        out += "\\\\";
        break;
      case '"':
        out += "\\\"";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
      default:
        out.push_back(ch);
        break;
    }
  }
  out.push_back('"');
  return out;
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

  // Allow running `./build/unode examples/foo.js` from repo root.
  const std::filesystem::path repo_fallback = std::filesystem::path("unode") / direct;
  if (std::filesystem::exists(repo_fallback)) {
    return repo_fallback.string();
  }
  return direct.string();
}

}  // namespace

int UnodeRunCliScript(const char* script_path, std::string* error_out) {
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
        return UnodeRunScriptFileWithLoop(env, resolved_script_path.c_str(), error_out, true);
      },
      error_out);
}

int UnodeRunCli(int argc, const char* const* argv, std::string* error_out) {
  if (error_out != nullptr) {
    error_out->clear();
  }
  if (argv == nullptr || argc < 2) {
    UnodeSetExecArgv({});
    UnodeSetScriptArgv({});
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
    if (error_out != nullptr) {
      *error_out = "Interactive mode is not implemented";
    }
    return 1;
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
    std::vector<std::string> exec_argv;
    exec_argv.reserve(static_cast<size_t>(mode_index));
    for (int i = 1; i < mode_index; ++i) {
      if (argv[i] == nullptr) continue;
      const std::string token(argv[i]);
      if (!token.empty() && token[0] == '-') exec_argv.push_back(token);
    }
    UnodeSetExecArgv(exec_argv);
    UnodeSetScriptArgv(script_argv);
    const std::string code = argv[mode_index + 1];
    std::string flags_line;
    for (int i = 1; i < mode_index; ++i) {
      if (argv[i] == nullptr) continue;
      const std::string token(argv[i]);
      if (!token.empty() && token[0] == '-') {
        if (flags_line.empty()) flags_line = "// Flags:";
        flags_line += " " + token;
      }
    }
    std::string source;
    if (is_print_mode) {
      const std::string quoted = JsonQuote(code);
      source =
          "(function(){"
          "const __unodeVm=require('vm');"
          "try { if (typeof globalThis.http === 'undefined') globalThis.http = require('http'); } catch (_) {}"
          "const __hadVm=Object.prototype.hasOwnProperty.call(globalThis,'vm');"
          "const __oldVm=globalThis.vm;"
          "globalThis.vm=__unodeVm;"
          "const __unodeExpr=" + quoted + ";"
          "try {"
          "const __unodeResult=globalThis.eval(__unodeExpr);"
          "if (__unodeResult!==undefined) console.log(__unodeResult);"
          "} finally {"
          "if (__hadVm) globalThis.vm=__oldVm; else delete globalThis.vm;"
          "}"
          "})();";
    } else {
      source = code;
    }
    if (!flags_line.empty()) {
      source = flags_line + "\n" + source;
    }
    return RunWithFreshEnv(
        [&](napi_env env) {
          return UnodeRunScriptSourceWithLoop(env, source.c_str(), error_out, true);
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
  UnodeSetExecArgv(exec_argv);
  if (script_index >= argc || argv[script_index] == nullptr) {
    UnodeSetScriptArgv({});
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
  UnodeSetScriptArgv(script_argv);
  return UnodeRunCliScript(argv[script_index], error_out);
}
