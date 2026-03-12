#include "internal_binding/dispatch.h"

#include "internal_binding/helpers.h"

namespace internal_binding {

napi_value ResolveTraceEvents(napi_env env, const ResolveOptions& options) {
  if (options.callbacks.get_or_create_trace_events == nullptr) return Undefined(env);
  napi_value binding = options.callbacks.get_or_create_trace_events(env);
  return binding != nullptr ? binding : Undefined(env);
}

}  // namespace internal_binding
