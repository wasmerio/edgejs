#include "edge_worker_env.h"

#include <array>
#include <map>
#include <mutex>
#include <unordered_set>
#include <unordered_map>

#include <uv.h>

namespace {

struct WorkerEnvState {
  bool cleanup_hook_registered = false;
  bool cleanup_started = false;
  bool stop_requested = false;
  EdgeWorkerEnvConfig config;
  napi_ref binding_ref = nullptr;
  napi_ref env_message_port_ref = nullptr;
  std::unordered_set<int> unmanaged_fds;
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

void EmitProcessWarning(napi_env env, const std::string& message) {
  if (env == nullptr || message.empty()) return;
  napi_value global = nullptr;
  napi_value process = nullptr;
  napi_value emit_warning = nullptr;
  napi_value message_value = nullptr;
  if (napi_get_global(env, &global) != napi_ok ||
      global == nullptr ||
      napi_get_named_property(env, global, "process", &process) != napi_ok ||
      process == nullptr ||
      napi_get_named_property(env, process, "emitWarning", &emit_warning) != napi_ok ||
      emit_warning == nullptr ||
      napi_create_string_utf8(env, message.c_str(), NAPI_AUTO_LENGTH, &message_value) != napi_ok ||
      message_value == nullptr) {
    return;
  }
  napi_value ignored = nullptr;
  (void)napi_call_function(env, process, emit_warning, 1, &message_value, &ignored);
}

void CleanupWorkerEnvState(void* data) {
  napi_env env = static_cast<napi_env>(data);
  if (env == nullptr) return;

  std::unordered_set<int> unmanaged_fds;
  napi_ref binding_ref = nullptr;
  napi_ref env_message_port_ref = nullptr;
  {
    std::lock_guard<std::mutex> lock(g_worker_env_mu);
    auto it = g_worker_env_states.find(env);
    if (it == g_worker_env_states.end()) return;
    it->second.cleanup_started = true;
    it->second.stop_requested = true;
    unmanaged_fds.swap(it->second.unmanaged_fds);
    binding_ref = it->second.binding_ref;
    env_message_port_ref = it->second.env_message_port_ref;
    it->second.binding_ref = nullptr;
    it->second.env_message_port_ref = nullptr;
  }

  DeleteRefIfPresent(env, &binding_ref);
  DeleteRefIfPresent(env, &env_message_port_ref);

  for (const int fd : unmanaged_fds) {
    if (fd < 0) continue;
    uv_fs_t req{};
    (void)uv_fs_close(nullptr, &req, fd, nullptr);
    uv_fs_req_cleanup(&req);
  }
}

WorkerEnvState& EnsureWorkerEnvState(napi_env env) {
  auto& state = g_worker_env_states[env];
  if (!state.cleanup_hook_registered && !state.cleanup_started) {
    if (napi_add_env_cleanup_hook(env, CleanupWorkerEnvState, env) == napi_ok) {
      state.cleanup_hook_registered = true;
    }
  }
  return state;
}

WorkerEnvState* FindWorkerEnvState(napi_env env) {
  auto it = g_worker_env_states.find(env);
  return it == g_worker_env_states.end() ? nullptr : &it->second;
}

}  // namespace

void EdgeWorkerEnvConfigure(napi_env env, const EdgeWorkerEnvConfig& config) {
  if (env == nullptr) return;
  std::lock_guard<std::mutex> lock(g_worker_env_mu);
  auto& state = EnsureWorkerEnvState(env);
  state.config = config;
  state.stop_requested = false;
}

bool EdgeWorkerEnvGetConfig(napi_env env, EdgeWorkerEnvConfig* out) {
  if (env == nullptr || out == nullptr) return false;
  std::lock_guard<std::mutex> lock(g_worker_env_mu);
  WorkerEnvState* state = FindWorkerEnvState(env);
  if (state == nullptr) {
    *out = EdgeWorkerEnvConfig();
    return false;
  }
  *out = state->config;
  return true;
}

bool EdgeWorkerEnvIsMainThread(napi_env env) {
  EdgeWorkerEnvConfig config;
  EdgeWorkerEnvGetConfig(env, &config);
  return config.is_main_thread;
}

bool EdgeWorkerEnvIsInternalThread(napi_env env) {
  EdgeWorkerEnvConfig config;
  EdgeWorkerEnvGetConfig(env, &config);
  return config.is_internal_thread;
}

bool EdgeWorkerEnvOwnsProcessState(napi_env env) {
  EdgeWorkerEnvConfig config;
  EdgeWorkerEnvGetConfig(env, &config);
  return config.owns_process_state;
}

bool EdgeWorkerEnvSharesEnvironment(napi_env env) {
  EdgeWorkerEnvConfig config;
  EdgeWorkerEnvGetConfig(env, &config);
  return config.share_env;
}

bool EdgeWorkerEnvTracksUnmanagedFds(napi_env env) {
  EdgeWorkerEnvConfig config;
  EdgeWorkerEnvGetConfig(env, &config);
  return config.tracks_unmanaged_fds;
}

void EdgeWorkerEnvAddUnmanagedFd(napi_env env, int fd) {
  if (env == nullptr || fd < 0) return;
  std::string warning;
  {
    std::lock_guard<std::mutex> lock(g_worker_env_mu);
    auto& state = EnsureWorkerEnvState(env);
    if (!state.config.tracks_unmanaged_fds) return;
    auto [_, inserted] = state.unmanaged_fds.emplace(fd);
    if (!inserted) {
      warning = "File descriptor " + std::to_string(fd) + " opened in unmanaged mode twice";
    }
  }
  if (!warning.empty()) EmitProcessWarning(env, warning);
}

void EdgeWorkerEnvRemoveUnmanagedFd(napi_env env, int fd) {
  if (env == nullptr || fd < 0) return;
  std::string warning;
  {
    std::lock_guard<std::mutex> lock(g_worker_env_mu);
    auto& state = EnsureWorkerEnvState(env);
    if (!state.config.tracks_unmanaged_fds) return;
    const size_t removed = state.unmanaged_fds.erase(fd);
    if (removed == 0) {
      warning = "File descriptor " + std::to_string(fd) + " closed but not opened in unmanaged mode";
    }
  }
  if (!warning.empty()) EmitProcessWarning(env, warning);
}

bool EdgeWorkerEnvStopRequested(napi_env env) {
  if (env == nullptr) return false;
  std::lock_guard<std::mutex> lock(g_worker_env_mu);
  WorkerEnvState* state = FindWorkerEnvState(env);
  return state != nullptr && state->stop_requested;
}

int32_t EdgeWorkerEnvThreadId(napi_env env) {
  EdgeWorkerEnvConfig config;
  EdgeWorkerEnvGetConfig(env, &config);
  return config.thread_id;
}

std::string EdgeWorkerEnvThreadName(napi_env env) {
  EdgeWorkerEnvConfig config;
  EdgeWorkerEnvGetConfig(env, &config);
  return config.thread_name;
}

std::array<double, 4> EdgeWorkerEnvResourceLimits(napi_env env) {
  EdgeWorkerEnvConfig config;
  EdgeWorkerEnvGetConfig(env, &config);
  return config.resource_limits;
}

std::string EdgeWorkerEnvGetProcessTitle(napi_env env) {
  EdgeWorkerEnvConfig config;
  EdgeWorkerEnvGetConfig(env, &config);
  return config.local_process_title;
}

void EdgeWorkerEnvSetProcessTitle(napi_env env, const std::string& title) {
  if (env == nullptr) return;
  std::lock_guard<std::mutex> lock(g_worker_env_mu);
  auto& state = EnsureWorkerEnvState(env);
  state.config.local_process_title = title;
}

uint32_t EdgeWorkerEnvGetDebugPort(napi_env env) {
  EdgeWorkerEnvConfig config;
  EdgeWorkerEnvGetConfig(env, &config);
  return config.local_debug_port;
}

void EdgeWorkerEnvSetDebugPort(napi_env env, uint32_t port) {
  if (env == nullptr) return;
  std::lock_guard<std::mutex> lock(g_worker_env_mu);
  auto& state = EnsureWorkerEnvState(env);
  state.config.local_debug_port = port;
}

std::map<std::string, std::string> EdgeWorkerEnvSnapshotEnvVars(napi_env env) {
  if (env == nullptr) return {};
  std::lock_guard<std::mutex> lock(g_worker_env_mu);
  WorkerEnvState* state = FindWorkerEnvState(env);
  return state != nullptr ? state->config.env_vars : std::map<std::string, std::string>{};
}

void EdgeWorkerEnvSetLocalEnvVar(napi_env env, const std::string& key, const std::string& value) {
  if (env == nullptr || key.empty()) return;
  std::lock_guard<std::mutex> lock(g_worker_env_mu);
  auto& state = EnsureWorkerEnvState(env);
  state.config.env_vars[key] = value;
}

void EdgeWorkerEnvUnsetLocalEnvVar(napi_env env, const std::string& key) {
  if (env == nullptr || key.empty()) return;
  std::lock_guard<std::mutex> lock(g_worker_env_mu);
  auto& state = EnsureWorkerEnvState(env);
  state.config.env_vars.erase(key);
}

void EdgeWorkerEnvRequestStop(napi_env env) {
  if (env == nullptr) return;
  std::lock_guard<std::mutex> lock(g_worker_env_mu);
  auto& state = EnsureWorkerEnvState(env);
  state.stop_requested = true;
}

void EdgeWorkerEnvRunCleanup(napi_env env) {
  if (env == nullptr) return;
  {
    std::lock_guard<std::mutex> lock(g_worker_env_mu);
    WorkerEnvState* state = FindWorkerEnvState(env);
    if (state == nullptr) return;
    if (state->cleanup_hook_registered) {
      (void)napi_remove_env_cleanup_hook(env, CleanupWorkerEnvState, env);
      state->cleanup_hook_registered = false;
    }
  }
  CleanupWorkerEnvState(env);
}

void EdgeWorkerEnvForget(napi_env env) {
  if (env == nullptr) return;
  std::lock_guard<std::mutex> lock(g_worker_env_mu);
  g_worker_env_states.erase(env);
}

napi_value EdgeWorkerEnvGetBinding(napi_env env) {
  if (env == nullptr) return nullptr;
  std::lock_guard<std::mutex> lock(g_worker_env_mu);
  WorkerEnvState* state = FindWorkerEnvState(env);
  return state != nullptr ? GetRefValue(env, state->binding_ref) : nullptr;
}

void EdgeWorkerEnvSetBinding(napi_env env, napi_value binding) {
  if (env == nullptr) return;
  std::lock_guard<std::mutex> lock(g_worker_env_mu);
  auto& state = EnsureWorkerEnvState(env);
  SetRefValue(env, &state.binding_ref, binding);
}

napi_value EdgeWorkerEnvGetEnvMessagePort(napi_env env) {
  if (env == nullptr) return nullptr;
  std::lock_guard<std::mutex> lock(g_worker_env_mu);
  WorkerEnvState* state = FindWorkerEnvState(env);
  return state != nullptr ? GetRefValue(env, state->env_message_port_ref) : nullptr;
}

void EdgeWorkerEnvSetEnvMessagePort(napi_env env, napi_value port) {
  if (env == nullptr) return;
  std::lock_guard<std::mutex> lock(g_worker_env_mu);
  auto& state = EnsureWorkerEnvState(env);
  SetRefValue(env, &state.env_message_port_ref, port);
}

internal_binding::EdgeMessagePortDataPtr EdgeWorkerEnvGetEnvMessagePortData(napi_env env) {
  EdgeWorkerEnvConfig config;
  EdgeWorkerEnvGetConfig(env, &config);
  return config.env_message_port_data;
}
