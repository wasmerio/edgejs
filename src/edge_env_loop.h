#ifndef EDGE_ENV_LOOP_H_
#define EDGE_ENV_LOOP_H_

#include "node_api.h"

#include <uv.h>

napi_status EdgeEnsureEnvLoop(napi_env env, uv_loop_t** loop_out);
uv_loop_t* EdgeGetEnvLoop(napi_env env);
uv_loop_t* EdgeGetExistingEnvLoop(napi_env env);

// unofficial_napi_env_hooks' env_cleanup_callback: runs the env cleanup hooks
// (napi_add_env_cleanup_hook) added while no Environment was attached, inside
// napi's release of `env`, and returns whether it ran any. An attached
// Environment runs its own in RunCleanup.
bool EdgeRunDetachedEnvCleanup(napi_env env, void* data);

#endif  // EDGE_ENV_LOOP_H_
