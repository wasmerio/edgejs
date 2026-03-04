#include "internal_binding/dispatch.h"

#include "internal_binding/helpers.h"

namespace internal_binding {

napi_value ResolveStreamWrap(napi_env env, const ResolveOptions& options) {
  if (options.callbacks.resolve_binding == nullptr) return Undefined(env);
  napi_value out = options.callbacks.resolve_binding(env, options.state, "stream_wrap");
  return out != nullptr ? out : Undefined(env);
}

}  // namespace internal_binding
