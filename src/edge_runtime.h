#ifndef EDGE_RUNTIME_H_
#define EDGE_RUNTIME_H_

#include <string>
#include <vector>

#include "node_api.h"

#if !defined(EDGE_ENABLE_STARTUP_PROFILE)
#define EDGE_ENABLE_STARTUP_PROFILE 0
#endif

enum class EdgeBootstrapProfile {
  kMainModule,
  kRepl,
  kTest,
  kWatch,
  kWorker,
};

struct EdgeExecutionCapabilities {
  EdgeBootstrapProfile bootstrap_profile = EdgeBootstrapProfile::kMainModule;
  bool requires_eager_openssl_init = false;
  bool requires_ipc_or_cluster_bootstrapping = false;
};

napi_status EdgeInstallConsole(napi_env env);
napi_status EdgeInstallProcessObject(napi_env env,
                                      const std::string& current_script_path,
                                      const std::vector<std::string>& exec_argv,
                                      const std::vector<std::string>& script_argv,
                                      const std::string& process_title);
napi_status EdgeInstallUnofficialNapiTestingUntilGc(napi_env env, napi_value target);
int EdgeRunScriptSource(napi_env env, const char* source_text, std::string* error_out);
int EdgeRunScriptSourceWithLoop(napi_env env,
                               const char* source_text,
                               std::string* error_out,
                               bool keep_event_loop_alive,
                               const char* native_main_builtin_id = nullptr);
int EdgeRunScriptFile(napi_env env, const char* script_path, std::string* error_out);
int EdgeRunScriptFileWithLoop(napi_env env,
                               const char* script_path,
                               std::string* error_out,
                               bool keep_event_loop_alive,
                               const char* native_main_builtin_id = nullptr);
int EdgeRunWorkerThreadMain(napi_env env,
                           const std::vector<std::string>& exec_argv,
                           std::string* error_out);
bool EdgeInitializeOpenSslForCli(std::string* error_out);
void EdgeSetCurrentScriptPath(const std::string& script_path);
void EdgeSetScriptArgv(const std::vector<std::string>& script_argv);
void EdgeSetExecArgv(const std::vector<std::string>& exec_argv);
void EdgeSetExecutionCapabilities(const EdgeExecutionCapabilities& capabilities);
const EdgeExecutionCapabilities& EdgeGetExecutionCapabilities();
#if EDGE_ENABLE_STARTUP_PROFILE
bool EdgeStartupProfileCompiledIn();
void EdgeStartupProfileReset();
bool EdgeStartupProfileEnabled();
void EdgeStartupProfileAddDuration(const char* key, double duration_ms);
void EdgeStartupProfileCaptureRuntimeState(napi_env env);
void EdgeStartupProfileEmit();
#else
inline bool EdgeStartupProfileCompiledIn() { return false; }
inline void EdgeStartupProfileReset() {}
inline bool EdgeStartupProfileEnabled() { return false; }
inline void EdgeStartupProfileAddDuration(const char* /*key*/, double /*duration_ms*/) {}
inline void EdgeStartupProfileCaptureRuntimeState(napi_env /*env*/) {}
inline void EdgeStartupProfileEmit() {}
#endif
bool EdgeExecArgvHasFlag(const char* flag);
bool EdgeReadExecArgvUint64Option(const char* prefix, uint64_t* out, bool* found);
bool EdgeFinalizeFatalExceptionNow(napi_env env,
                                   napi_value exception,
                                   int default_exit_code = 1,
                                   const std::string& exception_line = {},
                                   const std::string& thrown_at = {});
void EdgePrepareProcessExit(napi_env env, int exit_code);

enum EdgeMakeCallbackFlags : int {
  kEdgeMakeCallbackNone = 0,
  // Mirrors Node's InternalCallbackScope::kSkipTaskQueues for critical paths
  // like HTTP parser callbacks that must not re-enter JS tick processing.
  kEdgeMakeCallbackSkipTaskQueues = 1 << 0,
};

napi_status EdgeMakeCallbackWithFlags(napi_env env,
                                     napi_value recv,
                                     napi_value callback,
                                     size_t argc,
                                     napi_value* argv,
                                     napi_value* result,
                                     int flags);
napi_status EdgeCallCallbackWithDomain(napi_env env,
                                      napi_value recv,
                                      napi_value callback,
                                      size_t argc,
                                      napi_value* argv,
                                      napi_value* result);

napi_status EdgeMakeCallback(napi_env env,
                              napi_value recv,
                              napi_value callback,
                              size_t argc,
                              napi_value* argv,
                              napi_value* result);
// Mirrors the top-level task-queue checkpoint that Node runs when unwinding an
// InternalCallbackScope. Use this after settling native promises from libuv
// callbacks that did not enter JS through EdgeMakeCallback().
napi_status EdgeRunCallbackScopeCheckpoint(napi_env env);
bool EdgeHandlePendingExceptionNow(napi_env env, bool* handled_out);

#endif  // EDGE_RUNTIME_H_
