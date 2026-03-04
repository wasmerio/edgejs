#ifndef UBI_PROCESS_H_
#define UBI_PROCESS_H_

#include <string>
#include <vector>

#include "node_api.h"

napi_status UbiInstallProcessObject(napi_env env,
                                      const std::string& current_script_path,
                                      const std::vector<std::string>& exec_argv,
                                      const std::vector<std::string>& script_argv,
                                      const std::string& process_title);

napi_value UbiGetProcessMethodsBinding(napi_env env);
napi_value UbiGetReportBinding(napi_env env);

#endif  // UBI_PROCESS_H_
