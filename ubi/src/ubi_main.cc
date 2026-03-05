#include <iostream>
#include <iterator>
#include <string>

#if defined(_WIN32)
#include <io.h>
#else
#include <unistd.h>
#endif

#include "ubi_cli.h"
#include "ubi_process.h"
#include "ubi_runtime.h"
#include "unofficial_napi.h"

int main(int argc, char** argv) {
  UbiInitializeCliProcess();
  if (argc < 2) {
#if defined(_WIN32)
    const bool stdin_is_tty = _isatty(_fileno(stdin)) != 0;
#else
    const bool stdin_is_tty = isatty(STDIN_FILENO) != 0;
#endif
    if (!stdin_is_tty) {
      std::string source((std::istreambuf_iterator<char>(std::cin)), std::istreambuf_iterator<char>());

      std::string error;
      napi_env env = nullptr;
      void* env_scope = nullptr;
      const napi_status create_status = unofficial_napi_create_env(8, &env, &env_scope);
      if (create_status != napi_ok || env == nullptr || env_scope == nullptr) {
        std::cerr << "Failed to initialize runtime environment\n";
        return 1;
      }

      if (argv != nullptr && argc > 0 && argv[0] != nullptr) {
        UbiSetProcessArgv0(argv[0]);
      }
      UbiSetExecArgv({});
      UbiSetScriptArgv({});

      int exit_code = 0;
      if (!source.empty()) {
        exit_code = UbiRunScriptSourceWithLoop(env, source.c_str(), &error, true);
      }

      const napi_status release_status = unofficial_napi_release_env(env_scope);
      if (release_status != napi_ok) {
        std::cerr << "Failed to release runtime environment\n";
        return 1;
      }

      if (!error.empty()) {
        std::cerr << error << "\n";
      }
      return exit_code;
    }
  }

  std::string error;
  const int exit_code = UbiRunCli(argc, argv, &error);
  const bool is_process_exit_marker =
      error.rfind("process.exit(", 0) == 0 && !error.empty() && error.back() == ')';
  if (!error.empty() && !is_process_exit_marker) {
    std::cerr << error << "\n";
  }
  return exit_code;
}
