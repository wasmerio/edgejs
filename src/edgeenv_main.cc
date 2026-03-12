#include <iostream>
#include <string>

#include "edge_cli.h"

int main(int argc, char** argv) {
  EdgeInitializeCliProcess();
  std::string error;
  const int exit_code = EdgeRunEnvCli(argc, argv, &error);
  if (!error.empty()) {
    std::cerr << error << "\n";
  }
  return exit_code;
}
