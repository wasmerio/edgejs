#ifndef EDGE_WORKER_ENV_H_
#define EDGE_WORKER_ENV_H_

#include <array>
#include <map>
#include <string>

#include "node_api.h"
#include "internal_binding/binding_messaging.h"

struct EdgeWorkerEnvConfig {
  bool is_main_thread = true;
  bool is_internal_thread = false;
  bool owns_process_state = true;
  bool share_env = true;
  bool tracks_unmanaged_fds = false;
  int32_t thread_id = 0;
  std::string thread_name = "main";
  std::array<double, 4> resource_limits = {-1, -1, -1, -1};
  std::map<std::string, std::string> env_vars;
  internal_binding::EdgeMessagePortDataPtr env_message_port_data;
  std::string local_process_title;
  uint32_t local_debug_port = 0;
};

void EdgeWorkerEnvConfigure(napi_env env, const EdgeWorkerEnvConfig& config);
bool EdgeWorkerEnvGetConfig(napi_env env, EdgeWorkerEnvConfig* out);

bool EdgeWorkerEnvIsMainThread(napi_env env);
bool EdgeWorkerEnvIsInternalThread(napi_env env);
bool EdgeWorkerEnvOwnsProcessState(napi_env env);
bool EdgeWorkerEnvSharesEnvironment(napi_env env);
bool EdgeWorkerEnvTracksUnmanagedFds(napi_env env);
void EdgeWorkerEnvAddUnmanagedFd(napi_env env, int fd);
void EdgeWorkerEnvRemoveUnmanagedFd(napi_env env, int fd);
bool EdgeWorkerEnvStopRequested(napi_env env);
int32_t EdgeWorkerEnvThreadId(napi_env env);
std::string EdgeWorkerEnvThreadName(napi_env env);
std::array<double, 4> EdgeWorkerEnvResourceLimits(napi_env env);
std::string EdgeWorkerEnvGetProcessTitle(napi_env env);
void EdgeWorkerEnvSetProcessTitle(napi_env env, const std::string& title);
uint32_t EdgeWorkerEnvGetDebugPort(napi_env env);
void EdgeWorkerEnvSetDebugPort(napi_env env, uint32_t port);
std::map<std::string, std::string> EdgeWorkerEnvSnapshotEnvVars(napi_env env);
void EdgeWorkerEnvSetLocalEnvVar(napi_env env, const std::string& key, const std::string& value);
void EdgeWorkerEnvUnsetLocalEnvVar(napi_env env, const std::string& key);
void EdgeWorkerEnvRequestStop(napi_env env);
void EdgeWorkerEnvForget(napi_env env);
void EdgeWorkerEnvRunCleanup(napi_env env);

napi_value EdgeWorkerEnvGetBinding(napi_env env);
void EdgeWorkerEnvSetBinding(napi_env env, napi_value binding);

napi_value EdgeWorkerEnvGetEnvMessagePort(napi_env env);
void EdgeWorkerEnvSetEnvMessagePort(napi_env env, napi_value port);
internal_binding::EdgeMessagePortDataPtr EdgeWorkerEnvGetEnvMessagePortData(napi_env env);

#endif  // EDGE_WORKER_ENV_H_
