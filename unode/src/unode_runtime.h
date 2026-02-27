#ifndef UNODE_RUNTIME_H_
#define UNODE_RUNTIME_H_

#include <string>
#include <vector>

#include "js_native_api.h"

napi_status UnodeInstallConsole(napi_env env);
napi_status UnodeInstallProcessObject(napi_env env,
                                      const std::string& current_script_path,
                                      const std::vector<std::string>& exec_argv,
                                      const std::vector<std::string>& script_argv,
                                      const std::string& process_title);
napi_status UnodeInstallUnofficialNapiTestingUntilGc(napi_env env, napi_value target);
int UnodeRunScriptSource(napi_env env, const char* source_text, std::string* error_out);
int UnodeRunScriptFile(napi_env env, const char* script_path, std::string* error_out);
int UnodeRunScriptFileWithLoop(napi_env env,
                               const char* script_path,
                               std::string* error_out,
                               bool keep_event_loop_alive);
void UnodeSetScriptArgv(const std::vector<std::string>& script_argv);
napi_status UnodeMakeCallback(napi_env env,
                              napi_value recv,
                              napi_value callback,
                              size_t argc,
                              napi_value* argv,
                              napi_value* result);

#endif  // UNODE_RUNTIME_H_
