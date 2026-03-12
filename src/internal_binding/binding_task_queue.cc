#include "internal_binding/dispatch.h"

#include "internal_binding/helpers.h"

namespace internal_binding {

napi_value ResolveTaskQueue(napi_env env, const ResolveOptions& options) {
  if (options.callbacks.get_or_create_task_queue == nullptr) return Undefined(env);
  napi_value binding = options.callbacks.get_or_create_task_queue(env);
  return binding != nullptr ? binding : Undefined(env);
}

}  // namespace internal_binding
