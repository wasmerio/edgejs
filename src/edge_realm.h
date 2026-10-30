#ifndef EDGE_REALM_H_
#define EDGE_REALM_H_

#include "unofficial_napi.h"

// The env of the realm a context argument selects. A contextified object
// (from vm.createContext) selects its realm; a missing, null or undefined
// argument selects `env`'s own realm. Returns null when the object belongs to
// no realm env.
inline napi_env EdgeRealmEnvForContext(napi_env env, napi_value context) {
  if (context == nullptr) return env;
  napi_valuetype type = napi_undefined;
  if (napi_typeof(env, context, &type) != napi_ok) return nullptr;
  if (type == napi_undefined || type == napi_null) return env;
  napi_env realm = nullptr;
  if (unofficial_napi_get_realm(env, context, &realm) != napi_ok) return nullptr;
  return realm;
}

#endif  // EDGE_REALM_H_
