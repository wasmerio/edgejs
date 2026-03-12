#include "edge_active_resource.h"

#include <algorithm>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace {

struct ActiveHandleEntry {
  napi_ref keepalive_ref = nullptr;
  std::string resource_name;
  EdgeActiveHandleHasRef has_ref = nullptr;
  EdgeActiveHandleGetOwner get_owner = nullptr;
  void* data = nullptr;
};

struct ActiveRequestEntry {
  napi_ref req_ref = nullptr;
  std::string resource_name;
};

struct ActiveResourceState {
  std::vector<ActiveHandleEntry*> handles;
  std::vector<ActiveRequestEntry*> requests;
};

std::unordered_map<napi_env, ActiveResourceState> g_active_resource_states;
std::unordered_set<napi_env> g_active_resource_cleanup_hook_registered;

void EnsureActiveResourceCleanupHook(napi_env env);

ActiveResourceState& GetState(napi_env env) {
  EnsureActiveResourceCleanupHook(env);
  return g_active_resource_states[env];
}

void DeleteRef(napi_env env, napi_ref* ref) {
  if (env == nullptr || ref == nullptr || *ref == nullptr) return;
  napi_delete_reference(env, *ref);
  *ref = nullptr;
}

void OnActiveResourceEnvCleanup(void* data) {
  napi_env env = static_cast<napi_env>(data);
  g_active_resource_cleanup_hook_registered.erase(env);

  auto it = g_active_resource_states.find(env);
  if (it == g_active_resource_states.end()) return;
  for (ActiveHandleEntry* entry : it->second.handles) {
    if (entry == nullptr) continue;
    DeleteRef(env, &entry->keepalive_ref);
    delete entry;
  }
  for (ActiveRequestEntry* entry : it->second.requests) {
    if (entry == nullptr) continue;
    DeleteRef(env, &entry->req_ref);
    delete entry;
  }
  it->second.handles.clear();
  it->second.requests.clear();
  g_active_resource_states.erase(it);
}

void EnsureActiveResourceCleanupHook(napi_env env) {
  if (env == nullptr) return;
  auto [it, inserted] = g_active_resource_cleanup_hook_registered.emplace(env);
  if (!inserted) return;
  if (napi_add_env_cleanup_hook(env, OnActiveResourceEnvCleanup, env) != napi_ok) {
    g_active_resource_cleanup_hook_registered.erase(it);
  }
}

napi_value GetRefValue(napi_env env, napi_ref ref) {
  if (env == nullptr || ref == nullptr) return nullptr;
  napi_value value = nullptr;
  if (napi_get_reference_value(env, ref, &value) != napi_ok || value == nullptr) return nullptr;
  return value;
}

void AppendArrayValue(napi_env env, napi_value array, uint32_t* index, napi_value value) {
  if (env == nullptr || array == nullptr || index == nullptr || value == nullptr) return;
  napi_set_element(env, array, (*index)++, value);
}

void AppendStringValue(napi_env env, napi_value array, uint32_t* index, const std::string& value) {
  if (env == nullptr || array == nullptr || index == nullptr || value.empty()) return;
  napi_value str = nullptr;
  if (napi_create_string_utf8(env, value.c_str(), value.size(), &str) != napi_ok || str == nullptr) return;
  AppendArrayValue(env, array, index, str);
}

napi_value CreateArray(napi_env env) {
  napi_value out = nullptr;
  napi_create_array(env, &out);
  return out;
}

}  // namespace

void* EdgeRegisterActiveHandle(napi_env env,
                              napi_value keepalive_owner,
                              const char* resource_name,
                              EdgeActiveHandleHasRef has_ref,
                              EdgeActiveHandleGetOwner get_owner,
                              void* data) {
  if (env == nullptr || keepalive_owner == nullptr || resource_name == nullptr || has_ref == nullptr) return nullptr;
  auto* entry = new ActiveHandleEntry();
  entry->resource_name = resource_name;
  entry->has_ref = has_ref;
  entry->get_owner = get_owner;
  entry->data = data;
  if (napi_create_reference(env, keepalive_owner, 1, &entry->keepalive_ref) != napi_ok || entry->keepalive_ref == nullptr) {
    delete entry;
    return nullptr;
  }
  GetState(env).handles.push_back(entry);
  return entry;
}

void EdgeUnregisterActiveHandle(napi_env env, void* token) {
  if (env == nullptr || token == nullptr) return;
  ActiveResourceState& state = GetState(env);
  auto* entry = static_cast<ActiveHandleEntry*>(token);
  auto it = std::find(state.handles.begin(), state.handles.end(), entry);
  if (it == state.handles.end()) return;
  DeleteRef(env, &entry->keepalive_ref);
  state.handles.erase(it);
  delete entry;
}

void EdgeTrackActiveRequest(napi_env env, napi_value req, const char* resource_name) {
  if (env == nullptr || req == nullptr || resource_name == nullptr) return;
  auto* entry = new ActiveRequestEntry();
  entry->resource_name = resource_name;
  if (napi_create_reference(env, req, 1, &entry->req_ref) != napi_ok || entry->req_ref == nullptr) {
    delete entry;
    return;
  }
  GetState(env).requests.push_back(entry);
}

void EdgeUntrackActiveRequest(napi_env env, napi_value req) {
  if (env == nullptr || req == nullptr) return;
  ActiveResourceState& state = GetState(env);
  for (auto it = state.requests.begin(); it != state.requests.end(); ++it) {
    auto* entry = *it;
    napi_value current = GetRefValue(env, entry->req_ref);
    if (current == nullptr) continue;
    bool same = false;
    if (napi_strict_equals(env, current, req, &same) != napi_ok || !same) continue;
    DeleteRef(env, &entry->req_ref);
    state.requests.erase(it);
    delete entry;
    return;
  }
}

napi_value EdgeGetActiveHandlesArray(napi_env env) {
  napi_value out = CreateArray(env);
  if (out == nullptr) return nullptr;

  uint32_t index = 0;
  ActiveResourceState& state = GetState(env);
  for (ActiveHandleEntry* entry : state.handles) {
    if (entry == nullptr || !entry->has_ref(entry->data)) continue;
    napi_value owner =
        entry->get_owner != nullptr ? entry->get_owner(env, entry->data) : GetRefValue(env, entry->keepalive_ref);
    if (owner == nullptr) owner = GetRefValue(env, entry->keepalive_ref);
    if (owner == nullptr) continue;
    AppendArrayValue(env, out, &index, owner);
  }
  return out;
}

napi_value EdgeGetActiveRequestsArray(napi_env env) {
  napi_value out = CreateArray(env);
  if (out == nullptr) return nullptr;

  uint32_t index = 0;
  ActiveResourceState& state = GetState(env);
  for (ActiveRequestEntry* entry : state.requests) {
    if (entry == nullptr) continue;
    napi_value req = GetRefValue(env, entry->req_ref);
    if (req == nullptr) continue;
    AppendArrayValue(env, out, &index, req);
  }
  return out;
}

napi_value EdgeGetActiveResourcesInfoArray(napi_env env) {
  napi_value out = CreateArray(env);
  if (out == nullptr) return nullptr;

  uint32_t index = 0;
  ActiveResourceState& state = GetState(env);
  for (ActiveRequestEntry* entry : state.requests) {
    if (entry == nullptr) continue;
    if (GetRefValue(env, entry->req_ref) == nullptr) continue;
    AppendStringValue(env, out, &index, entry->resource_name);
  }
  for (ActiveHandleEntry* entry : state.handles) {
    if (entry == nullptr || !entry->has_ref(entry->data)) continue;
    napi_value owner =
        entry->get_owner != nullptr ? entry->get_owner(env, entry->data) : GetRefValue(env, entry->keepalive_ref);
    if (owner == nullptr) owner = GetRefValue(env, entry->keepalive_ref);
    if (owner == nullptr) continue;
    AppendStringValue(env, out, &index, entry->resource_name);
  }
  return out;
}
