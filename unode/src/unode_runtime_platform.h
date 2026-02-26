#ifndef UNODE_RUNTIME_PLATFORM_H_
#define UNODE_RUNTIME_PLATFORM_H_

#include "js_native_api.h"

// Engine-adapter boundary for runtime task draining.
// The current build provides a V8-backed implementation; future engines can
// replace it without changing unode runtime loop logic.
napi_status UnodeRuntimePlatformDrainTasks(napi_env env);

#endif  // UNODE_RUNTIME_PLATFORM_H_
