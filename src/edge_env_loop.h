#ifndef EDGE_ENV_LOOP_H_
#define EDGE_ENV_LOOP_H_

#include "node_api.h"
#include "unofficial_napi.h"

#include <uv.h>

napi_status EdgeEnsureEnvLoop(napi_env env, uv_loop_t** loop_out);
uv_loop_t* EdgeGetEnvLoop(napi_env env);
uv_loop_t* EdgeGetExistingEnvLoop(napi_env env);

// Releases `env` with unofficial_napi_release_env, after running the env
// cleanup hooks (napi_add_env_cleanup_hook) added while no Environment was
// attached to it; an attached Environment runs its own in RunCleanup. Every
// release of an env Edge made goes through this.
napi_status EdgeReleaseEnv(napi_env env,
                           unofficial_napi_env_owner owner,
                           uv_loop_t* loop_or_null);

#endif  // EDGE_ENV_LOOP_H_
