#ifndef EDGE_TIMERS_HOST_H_
#define EDGE_TIMERS_HOST_H_

#include "node_api.h"

napi_value EdgeInstallTimersHostBinding(napi_env env);
napi_status EdgeInitializeTimersHost(napi_env env);
int32_t EdgeGetActiveTimeoutCount(napi_env env);
uint32_t EdgeGetActiveImmediateRefCount(napi_env env);
void EdgeEnsureTimersImmediatePump(napi_env env);
void EdgeToggleImmediateRefFromNative(napi_env env, bool ref);
void EdgeRunTimersHostEnvCleanup(napi_env env);

#endif  // EDGE_TIMERS_HOST_H_
