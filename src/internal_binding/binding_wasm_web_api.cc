#include "internal_binding/dispatch.h"

#include <unordered_map>
#include <unordered_set>

#include "internal_binding/helpers.h"

namespace internal_binding {

namespace {

struct WasmWebApiState {
  napi_ref binding_ref = nullptr;
  napi_ref implementation_ref = nullptr;
};

std::unordered_map<napi_env, WasmWebApiState> g_wasm_web_api_states;
std::unordered_set<napi_env> g_wasm_web_api_cleanup_hook_registered;

void DeleteRefIfPresent(napi_env env, napi_ref* ref) {
  if (env == nullptr || ref == nullptr || *ref == nullptr) return;
  napi_delete_reference(env, *ref);
  *ref = nullptr;
}

void OnWasmWebApiEnvCleanup(void* data) {
  napi_env env = static_cast<napi_env>(data);
  g_wasm_web_api_cleanup_hook_registered.erase(env);

  auto it = g_wasm_web_api_states.find(env);
  if (it == g_wasm_web_api_states.end()) return;
  DeleteRefIfPresent(env, &it->second.binding_ref);
  DeleteRefIfPresent(env, &it->second.implementation_ref);
  g_wasm_web_api_states.erase(it);
}

void EnsureWasmWebApiCleanupHook(napi_env env) {
  if (env == nullptr) return;
  auto [it, inserted] = g_wasm_web_api_cleanup_hook_registered.emplace(env);
  if (!inserted) return;
  if (napi_add_env_cleanup_hook(env, OnWasmWebApiEnvCleanup, env) != napi_ok) {
    g_wasm_web_api_cleanup_hook_registered.erase(it);
  }
}

napi_value SetImplementationCallback(napi_env env, napi_callback_info info) {
  EnsureWasmWebApiCleanupHook(env);
  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  if (napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr) != napi_ok) {
    return nullptr;
  }

  auto& state = g_wasm_web_api_states[env];
  DeleteRefIfPresent(env, &state.implementation_ref);

  if (argc >= 1 && argv[0] != nullptr) {
    napi_valuetype type = napi_undefined;
    if (napi_typeof(env, argv[0], &type) == napi_ok && type == napi_function) {
      napi_create_reference(env, argv[0], 1, &state.implementation_ref);
    }
  }
  return Undefined(env);
}

napi_value GetCachedWasmWebApi(napi_env env) {
  auto it = g_wasm_web_api_states.find(env);
  if (it == g_wasm_web_api_states.end() || it->second.binding_ref == nullptr) return nullptr;
  napi_value out = nullptr;
  if (napi_get_reference_value(env, it->second.binding_ref, &out) != napi_ok || out == nullptr) return nullptr;
  return out;
}

}  // namespace

napi_value ResolveWasmWebApi(napi_env env, const ResolveOptions& /*options*/) {
  EnsureWasmWebApiCleanupHook(env);
  const napi_value undefined = Undefined(env);
  napi_value cached = GetCachedWasmWebApi(env);
  if (cached != nullptr) return cached;

  napi_value out = nullptr;
  if (napi_create_object(env, &out) != napi_ok || out == nullptr) return undefined;

  napi_value set_implementation = nullptr;
  if (napi_create_function(env,
                           "setImplementation",
                           NAPI_AUTO_LENGTH,
                           SetImplementationCallback,
                           nullptr,
                           &set_implementation) == napi_ok &&
      set_implementation != nullptr) {
    napi_set_named_property(env, out, "setImplementation", set_implementation);
  }

  auto& state = g_wasm_web_api_states[env];
  DeleteRefIfPresent(env, &state.binding_ref);
  napi_create_reference(env, out, 1, &state.binding_ref);
  return out;
}

}  // namespace internal_binding
