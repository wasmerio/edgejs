#include "internal_binding/dispatch.h"

#include "internal_binding/helpers.h"

namespace internal_binding {

napi_value ResolveErrors(napi_env env, const ResolveOptions& options) {
  if (options.callbacks.get_or_create_errors == nullptr) return Undefined(env);
  napi_value binding = options.callbacks.get_or_create_errors(env);
  return binding != nullptr ? binding : Undefined(env);
}

}  // namespace internal_binding
