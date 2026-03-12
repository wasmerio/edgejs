#include "internal_binding/dispatch.h"

#include "internal_binding/helpers.h"

namespace internal_binding {

napi_value ResolveModules(napi_env env, const ResolveOptions& options) {
  if (options.callbacks.resolve_modules == nullptr) return Undefined(env);
  napi_value out = options.callbacks.resolve_modules(env);
  return out != nullptr ? out : Undefined(env);
}

}  // namespace internal_binding
