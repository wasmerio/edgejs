#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#include "edge_cli.h"

extern "C" char** uv_setup_args(int argc, char** argv);

int main(int argc, char** argv) {
  argv = uv_setup_args(argc, argv);
  EdgeInitializeCliProcess();
  std::string error;
  const char* const* run_argv = const_cast<const char* const*>(argv);
  int run_argc = argc;
#if defined(EDGE_QUICKJS_WASIX_NPM_ALIAS)
  std::vector<const char*> alias_argv;
  if (argc > 0 && argv != nullptr && argv[0] != nullptr) {
    const char* basename = std::strrchr(argv[0], '/');
    basename = basename == nullptr ? argv[0] : basename + 1;
    if (std::strcmp(basename, "edge-npm-internal") == 0 &&
        (argc < 2 || argv[1] == nullptr ||
         std::strcmp(argv[1], "/npm/bin/npm-cli.js") != 0)) {
      // WASIX child spawns retain the command alias but omit the manifest's
      // main-args. Supply the npm entrypoint for pnpm's delegated commands.
      alias_argv.reserve(static_cast<size_t>(argc) + 2);
      alias_argv.push_back(argv[0]);
      alias_argv.push_back("/npm/bin/npm-cli.js");
      for (int i = 1; i < argc; ++i) alias_argv.push_back(argv[i]);
      alias_argv.push_back(nullptr);
      run_argv = alias_argv.data();
      run_argc = argc + 1;
    }
  }
#endif
  const int exit_code = EdgeRunCli(run_argc, run_argv, &error);
  if (!error.empty()) {
    std::cerr << error << "\n";
  }
  return exit_code;
}
