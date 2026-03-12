#include "internal_binding/dispatch.h"

#include "internal_binding/helpers.h"

namespace internal_binding {

napi_value ResolveTlsWrap(napi_env env, const ResolveOptions& options) {
  if (options.callbacks.resolve_binding == nullptr) return Undefined(env);
  napi_value out = options.callbacks.resolve_binding(env, options.state, "tls_wrap");
  if (out == nullptr || IsUndefined(env, out)) return Undefined(env);
  return out;
}

}  // namespace internal_binding
