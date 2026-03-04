#include "internal_binding/dispatch.h"

#include "internal_binding/helpers.h"

namespace internal_binding {

napi_value ResolveProcessMethods(napi_env env, const ResolveOptions& /*options*/) {
  napi_value binding = GetGlobalNamed(env, "__ubi_process_methods_binding");
  return (binding == nullptr || IsUndefined(env, binding)) ? Undefined(env) : binding;
}

}  // namespace internal_binding
