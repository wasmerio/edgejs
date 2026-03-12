#include "internal_binding/dispatch.h"

#include "internal_binding/helpers.h"

namespace internal_binding {

napi_value ResolveTimers(napi_env env, const ResolveOptions& options) {
  if (options.callbacks.resolve_binding == nullptr) return Undefined(env);
  napi_value timers_binding = options.callbacks.resolve_binding(env, options.state, "timers");
  return timers_binding != nullptr ? timers_binding : Undefined(env);
}

}  // namespace internal_binding
