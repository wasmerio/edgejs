#include "internal_binding/dispatch.h"

#include "internal_binding/helpers.h"

namespace internal_binding {

napi_value ResolveBuffer(napi_env env, const ResolveOptions& options) {
  if (options.callbacks.resolve_binding == nullptr) return Undefined(env);
  napi_value binding = options.callbacks.resolve_binding(env, options.state, "buffer");
  return (binding == nullptr || IsUndefined(env, binding)) ? Undefined(env) : binding;
}

}  // namespace internal_binding
