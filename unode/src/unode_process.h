#ifndef UNODE_PROCESS_H_
#define UNODE_PROCESS_H_

#include <string>
#include <vector>

#include "js_native_api.h"

napi_status UnodeInstallProcessObject(napi_env env,
                                      const std::string& current_script_path,
                                      const std::vector<std::string>& exec_argv,
                                      const std::string& process_title);

#endif  // UNODE_PROCESS_H_
