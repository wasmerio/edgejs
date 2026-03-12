#include "internal_binding/dispatch.h"

#include <unordered_map>

#include "internal_binding/helpers.h"
#include "unofficial_napi.h"

namespace internal_binding {

namespace {

struct AsyncContextFrameBindingState {
  napi_ref binding_ref = nullptr;
};

std::unordered_map<napi_env, AsyncContextFrameBindingState> g_async_context_frame_states;
std::unordered_map<napi_env, bool> g_async_context_frame_cleanup_installed;

AsyncContextFrameBindingState& GetState(napi_env env) {
  return g_async_context_frame_states[env];
}

void OnAsyncContextFrameEnvCleanup(void* arg) {
  napi_env env = static_cast<napi_env>(arg);
  auto installed_it = g_async_context_frame_cleanup_installed.find(env);
  if (installed_it != g_async_context_frame_cleanup_installed.end()) {
    g_async_context_frame_cleanup_installed.erase(installed_it);
  }

  auto it = g_async_context_frame_states.find(env);
  if (it == g_async_context_frame_states.end()) return;
  if (it->second.binding_ref != nullptr) {
    napi_delete_reference(env, it->second.binding_ref);
    it->second.binding_ref = nullptr;
  }
  g_async_context_frame_states.erase(it);
}

void EnsureAsyncContextFrameCleanupHook(napi_env env) {
  if (g_async_context_frame_cleanup_installed[env]) return;
  if (napi_add_env_cleanup_hook(env, OnAsyncContextFrameEnvCleanup, env) == napi_ok) {
    g_async_context_frame_cleanup_installed[env] = true;
  }
}

napi_value GetContinuationPreservedEmbedderData(napi_env env, napi_callback_info info) {
  (void)info;
  napi_value value = nullptr;
  if (unofficial_napi_get_continuation_preserved_embedder_data(env, &value) != napi_ok ||
      value == nullptr) {
    return Undefined(env);
  }
  return value;
}

napi_value SetContinuationPreservedEmbedderData(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);

  napi_value value = Undefined(env);
  if (argc >= 1 && argv[0] != nullptr) {
    value = argv[0];
  }
  (void)unofficial_napi_set_continuation_preserved_embedder_data(env, value);
  return Undefined(env);
}

}  // namespace

napi_value ResolveAsyncContextFrame(napi_env env, const ResolveOptions& options) {
  (void)options;
  EnsureAsyncContextFrameCleanupHook(env);
  AsyncContextFrameBindingState& state = GetState(env);
  if (state.binding_ref != nullptr) {
    napi_value cached = nullptr;
    if (napi_get_reference_value(env, state.binding_ref, &cached) == napi_ok && cached != nullptr) {
      return cached;
    }
  }

  napi_value binding = nullptr;
  if (napi_create_object(env, &binding) != napi_ok || binding == nullptr) return Undefined(env);

  napi_value getter = nullptr;
  napi_create_function(env,
                       "getContinuationPreservedEmbedderData",
                       NAPI_AUTO_LENGTH,
                       GetContinuationPreservedEmbedderData,
                       nullptr,
                       &getter);
  if (getter != nullptr) {
    napi_set_named_property(env, binding, "getContinuationPreservedEmbedderData", getter);
  }

  napi_value setter = nullptr;
  napi_create_function(env,
                       "setContinuationPreservedEmbedderData",
                       NAPI_AUTO_LENGTH,
                       SetContinuationPreservedEmbedderData,
                       nullptr,
                       &setter);
  if (setter != nullptr) {
    napi_set_named_property(env, binding, "setContinuationPreservedEmbedderData", setter);
  }

  if (state.binding_ref != nullptr) {
    napi_delete_reference(env, state.binding_ref);
    state.binding_ref = nullptr;
  }
  napi_create_reference(env, binding, 1, &state.binding_ref);

  return binding;
}

}  // namespace internal_binding
