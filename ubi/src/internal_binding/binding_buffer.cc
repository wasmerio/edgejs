#include "internal_binding/dispatch.h"

#include <unordered_map>

#include "internal_binding/helpers.h"

namespace internal_binding {

namespace {

struct BufferBindingState {
  napi_ref zero_fill_toggle_ref = nullptr;
};

std::unordered_map<napi_env, BufferBindingState> g_buffer_states;

napi_value BufferGetZeroFillToggle(napi_env env, napi_callback_info /*info*/) {
  auto& st = g_buffer_states[env];
  if (st.zero_fill_toggle_ref != nullptr) {
    napi_value existing = nullptr;
    if (napi_get_reference_value(env, st.zero_fill_toggle_ref, &existing) == napi_ok &&
        existing != nullptr) {
      return existing;
    }
    napi_delete_reference(env, st.zero_fill_toggle_ref);
    st.zero_fill_toggle_ref = nullptr;
  }

  void* data = nullptr;
  napi_value ab = nullptr;
  if (napi_create_arraybuffer(env, sizeof(uint32_t), &data, &ab) != napi_ok || data == nullptr ||
      ab == nullptr) {
    return Undefined(env);
  }
  static_cast<uint32_t*>(data)[0] = 1;

  napi_value ta = nullptr;
  if (napi_create_typedarray(env, napi_uint32_array, 1, ab, 0, &ta) != napi_ok || ta == nullptr) {
    return Undefined(env);
  }
  napi_create_reference(env, ta, 1, &st.zero_fill_toggle_ref);
  return ta;
}

void EnsureGetZeroFillToggle(napi_env env, napi_value binding) {
  bool has_property = false;
  if (napi_has_named_property(env, binding, "getZeroFillToggle", &has_property) != napi_ok ||
      !has_property) {
    napi_value fn = nullptr;
    if (napi_create_function(env,
                             "getZeroFillToggle",
                             NAPI_AUTO_LENGTH,
                             BufferGetZeroFillToggle,
                             nullptr,
                             &fn) == napi_ok &&
        fn != nullptr) {
      napi_set_named_property(env, binding, "getZeroFillToggle", fn);
    }
    return;
  }

  napi_value current = nullptr;
  if (napi_get_named_property(env, binding, "getZeroFillToggle", &current) != napi_ok ||
      current == nullptr) {
    return;
  }
  napi_valuetype type = napi_undefined;
  if (napi_typeof(env, current, &type) != napi_ok || type != napi_function) {
    napi_value fn = nullptr;
    if (napi_create_function(env,
                             "getZeroFillToggle",
                             NAPI_AUTO_LENGTH,
                             BufferGetZeroFillToggle,
                             nullptr,
                             &fn) == napi_ok &&
        fn != nullptr) {
      napi_set_named_property(env, binding, "getZeroFillToggle", fn);
    }
  }
}

}  // namespace

napi_value ResolveBuffer(napi_env env, const ResolveOptions& options) {
  if (options.callbacks.resolve_binding == nullptr) return Undefined(env);
  napi_value binding = options.callbacks.resolve_binding(env, options.state, "buffer");
  if (binding == nullptr || IsUndefined(env, binding)) return Undefined(env);
  EnsureGetZeroFillToggle(env, binding);
  return binding;
}

}  // namespace internal_binding
