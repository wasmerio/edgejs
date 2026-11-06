#ifndef EDGE_REALM_H_
#define EDGE_REALM_H_

#include "unofficial_napi.h"

// Edge's native bindings are created in the principal realm. Resolve a public
// vm context at the point of use rather than retaining a scoped realm handle.
// The context value keeps the realm alive throughout the operation.
inline napi_status EdgeGetRealmForContext(napi_env env,
                                         napi_value context,
                                         unofficial_napi_realm* realm_out) {
  if (env == nullptr || realm_out == nullptr) return napi_invalid_arg;
  *realm_out = nullptr;
  if (context == nullptr) return napi_ok;
  napi_valuetype type = napi_undefined;
  napi_status status = napi_typeof(env, context, &type);
  if (status != napi_ok) return status;
  if (type == napi_undefined || type == napi_null) return napi_ok;
  if (type != napi_object && type != napi_function) return napi_invalid_arg;
  status = unofficial_napi_get_realm(env, context, realm_out);
  if (status != napi_ok || *realm_out != nullptr) return status;

  // Internal callers may explicitly pass the principal global. An ordinary
  // object is not a vm context, even though get_realm returns NULL for both.
  napi_value global = nullptr;
  status = napi_get_global(env, &global);
  if (status != napi_ok) return status;
  bool is_global = false;
  status = napi_strict_equals(env, context, global, &is_global);
  if (status != napi_ok) return status;
  return is_global ? napi_ok : napi_invalid_arg;
}

#endif  // EDGE_REALM_H_
