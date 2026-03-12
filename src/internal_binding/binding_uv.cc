#include "internal_binding/dispatch.h"

#include "internal_binding/helpers.h"

namespace internal_binding {

napi_value ResolveUv(napi_env env, const ResolveOptions& options) {
  if (options.callbacks.resolve_uv == nullptr) return Undefined(env);
  napi_value binding = options.callbacks.resolve_uv(env);
  return binding != nullptr ? binding : Undefined(env);
}

}  // namespace internal_binding
