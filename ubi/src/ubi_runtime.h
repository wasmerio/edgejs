#ifndef UBI_RUNTIME_H_
#define UBI_RUNTIME_H_

#include <string>
#include <vector>

#include "node_api.h"

napi_status UbiInstallConsole(napi_env env);
napi_status UbiInstallProcessObject(napi_env env,
                                      const std::string& current_script_path,
                                      const std::vector<std::string>& exec_argv,
                                      const std::vector<std::string>& script_argv,
                                      const std::string& process_title);
napi_status UbiInstallUnofficialNapiTestingUntilGc(napi_env env, napi_value target);
int UbiRunScriptSource(napi_env env, const char* source_text, std::string* error_out);
int UbiRunScriptSourceWithLoop(napi_env env,
                               const char* source_text,
                               std::string* error_out,
                               bool keep_event_loop_alive);
int UbiRunScriptFile(napi_env env, const char* script_path, std::string* error_out);
int UbiRunScriptFileWithLoop(napi_env env,
                               const char* script_path,
                               std::string* error_out,
                               bool keep_event_loop_alive);
void UbiSetScriptArgv(const std::vector<std::string>& script_argv);
void UbiSetExecArgv(const std::vector<std::string>& exec_argv);

enum UbiMakeCallbackFlags : int {
  kUbiMakeCallbackNone = 0,
  // Mirrors Node's InternalCallbackScope::kSkipTaskQueues for critical paths
  // like HTTP parser callbacks that must not re-enter JS tick processing.
  kUbiMakeCallbackSkipTaskQueues = 1 << 0,
};

napi_status UbiMakeCallbackWithFlags(napi_env env,
                                     napi_value recv,
                                     napi_value callback,
                                     size_t argc,
                                     napi_value* argv,
                                     napi_value* result,
                                     int flags);

napi_status UbiMakeCallback(napi_env env,
                              napi_value recv,
                              napi_value callback,
                              size_t argc,
                              napi_value* argv,
                              napi_value* result);

#endif  // UBI_RUNTIME_H_
