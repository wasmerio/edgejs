#ifndef EDGE_ENV_LOOP_H_
#define EDGE_ENV_LOOP_H_

#include "node_api.h"

#include <uv.h>

inline bool EdgeIsCloseableHandle(const uv_handle_t* handle) {
  if (handle == nullptr) return false;
  switch (uv_handle_get_type(handle)) {
    case UV_UNKNOWN_HANDLE:
    case UV_HANDLE:
    case UV_STREAM:
    case UV_FILE:
      return false;
    default:
      return true;
  }
}

napi_status EdgeEnsureEnvLoop(napi_env env, uv_loop_t** loop_out);
uv_loop_t* EdgeGetEnvLoop(napi_env env);
uv_loop_t* EdgeGetExistingEnvLoop(napi_env env);

#endif  // EDGE_ENV_LOOP_H_
