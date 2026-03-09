#include "ubi_worker_env.h"

#include <array>
#include <map>
#include <mutex>
#include <unordered_map>

namespace {

struct WorkerEnvState {
  bool cleanup_hook_registered = false;
  bool stop_requested = false;
  UbiWorkerEnvConfig config;
  napi_ref binding_ref = nullptr;
  napi_ref env_message_port_ref = nullptr;
};

std::mutex g_worker_env_mu;
std::unordered_map<napi_env, WorkerEnvState> g_worker_env_states;

void DeleteRefIfPresent(napi_env env, napi_ref* ref) {
  if (ref == nullptr || *ref == nullptr) return;
  napi_delete_reference(env, *ref);
  *ref = nullptr;
}

napi_value GetRefValue(napi_env env, napi_ref ref) {
  if (ref == nullptr) return nullptr;
  napi_value out = nullptr;
  if (napi_get_reference_value(env, ref, &out) != napi_ok || out == nullptr) return nullptr;
  return out;
}

void SetRefValue(napi_env env, napi_ref* slot, napi_value value) {
  if (slot == nullptr) return;
  DeleteRefIfPresent(env, slot);
  if (value != nullptr) {
    napi_create_reference(env, value, 1, slot);
  }
}

void CleanupWorkerEnvState(void* data) {
  napi_env env = static_cast<napi_env>(data);
  if (env == nullptr) return;

  std::lock_guard<std::mutex> lock(g_worker_env_mu);
  auto it = g_worker_env_states.find(env);
  if (it == g_worker_env_states.end()) return;
  DeleteRefIfPresent(env, &it->second.binding_ref);
  DeleteRefIfPresent(env, &it->second.env_message_port_ref);
  g_worker_env_states.erase(it);
}

WorkerEnvState& EnsureWorkerEnvState(napi_env env) {
  auto& state = g_worker_env_states[env];
  if (!state.cleanup_hook_registered) {
    if (napi_add_env_cleanup_hook(env, CleanupWorkerEnvState, env) == napi_ok) {
      state.cleanup_hook_registered = true;
    }
  }
  return state;
}

}  // namespace

void UbiWorkerEnvConfigure(napi_env env, const UbiWorkerEnvConfig& config) {
  if (env == nullptr) return;
  std::lock_guard<std::mutex> lock(g_worker_env_mu);
  auto& state = EnsureWorkerEnvState(env);
  state.config = config;
  state.stop_requested = false;
}

bool UbiWorkerEnvGetConfig(napi_env env, UbiWorkerEnvConfig* out) {
  if (env == nullptr || out == nullptr) return false;
  std::lock_guard<std::mutex> lock(g_worker_env_mu);
  auto& state = EnsureWorkerEnvState(env);
  *out = state.config;
  return true;
}

bool UbiWorkerEnvIsMainThread(napi_env env) {
  UbiWorkerEnvConfig config;
  UbiWorkerEnvGetConfig(env, &config);
  return config.is_main_thread;
}

bool UbiWorkerEnvIsInternalThread(napi_env env) {
  UbiWorkerEnvConfig config;
  UbiWorkerEnvGetConfig(env, &config);
  return config.is_internal_thread;
}

bool UbiWorkerEnvOwnsProcessState(napi_env env) {
  UbiWorkerEnvConfig config;
  UbiWorkerEnvGetConfig(env, &config);
  return config.owns_process_state;
}

bool UbiWorkerEnvSharesEnvironment(napi_env env) {
  UbiWorkerEnvConfig config;
  UbiWorkerEnvGetConfig(env, &config);
  return config.share_env;
}

bool UbiWorkerEnvStopRequested(napi_env env) {
  if (env == nullptr) return false;
  std::lock_guard<std::mutex> lock(g_worker_env_mu);
  auto& state = EnsureWorkerEnvState(env);
  return state.stop_requested;
}

int32_t UbiWorkerEnvThreadId(napi_env env) {
  UbiWorkerEnvConfig config;
  UbiWorkerEnvGetConfig(env, &config);
  return config.thread_id;
}

std::string UbiWorkerEnvThreadName(napi_env env) {
  UbiWorkerEnvConfig config;
  UbiWorkerEnvGetConfig(env, &config);
  return config.thread_name;
}

std::array<double, 4> UbiWorkerEnvResourceLimits(napi_env env) {
  UbiWorkerEnvConfig config;
  UbiWorkerEnvGetConfig(env, &config);
  return config.resource_limits;
}

std::map<std::string, std::string> UbiWorkerEnvSnapshotEnvVars(napi_env env) {
  if (env == nullptr) return {};
  std::lock_guard<std::mutex> lock(g_worker_env_mu);
  auto& state = EnsureWorkerEnvState(env);
  return state.config.env_vars;
}

void UbiWorkerEnvSetLocalEnvVar(napi_env env, const std::string& key, const std::string& value) {
  if (env == nullptr || key.empty()) return;
  std::lock_guard<std::mutex> lock(g_worker_env_mu);
  auto& state = EnsureWorkerEnvState(env);
  state.config.env_vars[key] = value;
}

void UbiWorkerEnvUnsetLocalEnvVar(napi_env env, const std::string& key) {
  if (env == nullptr || key.empty()) return;
  std::lock_guard<std::mutex> lock(g_worker_env_mu);
  auto& state = EnsureWorkerEnvState(env);
  state.config.env_vars.erase(key);
}

void UbiWorkerEnvRequestStop(napi_env env) {
  if (env == nullptr) return;
  std::lock_guard<std::mutex> lock(g_worker_env_mu);
  auto& state = EnsureWorkerEnvState(env);
  state.stop_requested = true;
}

napi_value UbiWorkerEnvGetBinding(napi_env env) {
  if (env == nullptr) return nullptr;
  std::lock_guard<std::mutex> lock(g_worker_env_mu);
  auto& state = EnsureWorkerEnvState(env);
  return GetRefValue(env, state.binding_ref);
}

void UbiWorkerEnvSetBinding(napi_env env, napi_value binding) {
  if (env == nullptr) return;
  std::lock_guard<std::mutex> lock(g_worker_env_mu);
  auto& state = EnsureWorkerEnvState(env);
  SetRefValue(env, &state.binding_ref, binding);
}

napi_value UbiWorkerEnvGetEnvMessagePort(napi_env env) {
  if (env == nullptr) return nullptr;
  std::lock_guard<std::mutex> lock(g_worker_env_mu);
  auto& state = EnsureWorkerEnvState(env);
  return GetRefValue(env, state.env_message_port_ref);
}

void UbiWorkerEnvSetEnvMessagePort(napi_env env, napi_value port) {
  if (env == nullptr) return;
  std::lock_guard<std::mutex> lock(g_worker_env_mu);
  auto& state = EnsureWorkerEnvState(env);
  SetRefValue(env, &state.env_message_port_ref, port);
}

internal_binding::UbiMessagePortDataPtr UbiWorkerEnvGetEnvMessagePortData(napi_env env) {
  UbiWorkerEnvConfig config;
  UbiWorkerEnvGetConfig(env, &config);
  return config.env_message_port_data;
}
