#ifndef UBI_TIMERS_HOST_H_
#define UBI_TIMERS_HOST_H_

#include "node_api.h"

napi_value UbiInstallTimersHostBinding(napi_env env);
int32_t UbiGetActiveTimeoutCount(napi_env env);
uint32_t UbiGetActiveImmediateRefCount(napi_env env);
void UbiEnsureTimersImmediatePump(napi_env env);
void UbiToggleImmediateRefFromNative(napi_env env, bool ref);

#endif  // UBI_TIMERS_HOST_H_
