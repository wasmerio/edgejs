#include "internal_binding/dispatch.h"

#include "internal_binding/helpers.h"

namespace internal_binding {

namespace {

void SetNamedInt(napi_env env, napi_value obj, const char* key, int32_t value) {
  napi_value out = nullptr;
  if (napi_create_int32(env, value, &out) == napi_ok && out != nullptr) {
    napi_set_named_property(env, obj, key, out);
  }
}

void EnsureMeasureMemoryConstants(napi_env env, napi_value binding) {
  napi_value constants = nullptr;
  if (napi_get_named_property(env, binding, "constants", &constants) != napi_ok || constants == nullptr ||
      IsUndefined(env, constants)) {
    if (napi_create_object(env, &constants) != napi_ok || constants == nullptr) return;
    napi_set_named_property(env, binding, "constants", constants);
  }

  napi_value mm = nullptr;
  if (napi_get_named_property(env, constants, "measureMemory", &mm) != napi_ok || mm == nullptr ||
      IsUndefined(env, mm)) {
    if (napi_create_object(env, &mm) != napi_ok || mm == nullptr) return;
    napi_set_named_property(env, constants, "measureMemory", mm);
  }

  napi_value mode = nullptr;
  if (napi_get_named_property(env, mm, "mode", &mode) != napi_ok || mode == nullptr || IsUndefined(env, mode)) {
    if (napi_create_object(env, &mode) != napi_ok || mode == nullptr) return;
    napi_set_named_property(env, mm, "mode", mode);
  }
  SetNamedInt(env, mode, "SUMMARY", 0);
  SetNamedInt(env, mode, "DETAILED", 1);

  napi_value execution = nullptr;
  if (napi_get_named_property(env, mm, "execution", &execution) != napi_ok || execution == nullptr ||
      IsUndefined(env, execution)) {
    if (napi_create_object(env, &execution) != napi_ok || execution == nullptr) return;
    napi_set_named_property(env, mm, "execution", execution);
  }
  SetNamedInt(env, execution, "DEFAULT", 0);
  SetNamedInt(env, execution, "EAGER", 1);
}

napi_value ContextifyMeasureMemoryFallback(napi_env env, napi_callback_info info) {
  (void)info;
  napi_deferred deferred = nullptr;
  napi_value promise = nullptr;
  if (napi_create_promise(env, &deferred, &promise) != napi_ok || promise == nullptr) return Undefined(env);
  napi_value value = nullptr;
  napi_create_object(env, &value);
  napi_resolve_deferred(env, deferred, value != nullptr ? value : Undefined(env));
  return promise;
}

void EnsureMethod(napi_env env, napi_value binding, const char* name, napi_callback cb) {
  bool has = false;
  if (napi_has_named_property(env, binding, name, &has) != napi_ok || has) return;
  napi_value fn = nullptr;
  if (napi_create_function(env, name, NAPI_AUTO_LENGTH, cb, nullptr, &fn) == napi_ok && fn != nullptr) {
    napi_set_named_property(env, binding, name, fn);
  }
}

}  // namespace

napi_value ResolveContextify(napi_env env, const ResolveOptions& options) {
  if (options.callbacks.resolve_contextify == nullptr) return Undefined(env);
  napi_value out = options.callbacks.resolve_contextify(env);
  if (out == nullptr || IsUndefined(env, out)) return Undefined(env);

  EnsureMeasureMemoryConstants(env, out);
  EnsureMethod(env, out, "measureMemory", ContextifyMeasureMemoryFallback);
  return out;
}

}  // namespace internal_binding
