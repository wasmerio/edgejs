#ifndef EDGE_ENV_LOOP_H_
#define EDGE_ENV_LOOP_H_

#include "node_api.h"

#include <uv.h>

napi_status EdgeEnsureEnvLoop(napi_env env, uv_loop_t** loop_out);
uv_loop_t* EdgeGetEnvLoop(napi_env env);
uv_loop_t* EdgeGetExistingEnvLoop(napi_env env);

// The env's cleanup callback (unofficial_napi_set_env_cleanup_callback): runs the env cleanup hooks
// (napi_add_env_cleanup_hook) added while no Environment was attached, inside
// napi's release of `env`, and returns whether it ran any. An attached
// Environment runs its own in RunCleanup.
bool EdgeRunDetachedEnvCleanup(napi_env env, void* data);
// napi_add_env_cleanup_hook for Edge's own teardown: an env with no
// Environment keeps the hook for EdgeRunDetachedEnvCleanup whether or not its
// release will call that (as napi's own hooks were kept on the WASIX host,
// which never ran them). Addons' registrations are refused there instead.
napi_status EdgeAddEnvCleanupHook(napi_env env, napi_cleanup_hook fun, void* arg);

#endif  // EDGE_ENV_LOOP_H_
