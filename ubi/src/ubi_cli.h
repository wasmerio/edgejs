#ifndef UBI_CLI_H_
#define UBI_CLI_H_

#include <string>

void UbiInitializeCliProcess();
int UbiRunCli(int argc, const char* const* argv, std::string* error_out);
int UbiRunCliScript(const char* script_path, std::string* error_out);

#endif  // UBI_CLI_H_
