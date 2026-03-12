#include "internal_binding/dispatch.h"

#include "internal_binding/helpers.h"
#include "edge_encoding.h"

namespace internal_binding {

napi_value ResolveEncodingBinding(napi_env env, const ResolveOptions& options) {
  if (options.callbacks.resolve_binding == nullptr) return Undefined(env);
  napi_value binding = options.callbacks.resolve_binding(env, options.state, "encoding_binding");
  return (binding == nullptr || IsUndefined(env, binding)) ? Undefined(env) : binding;
}

}  // namespace internal_binding
