#ifndef EDGE_ENVIRONMENT_RUNTIME_H_
#define EDGE_ENVIRONMENT_RUNTIME_H_

#include "edge_environment.h"

using EdgeStartupTraceCallback = void (*)(void* data, const char* phase);

bool EdgeAttachEnvironmentForRuntime(napi_env env,
                                     const EdgeEnvironmentConfig* config = nullptr,
                                     EdgeStartupTraceCallback trace_callback = nullptr,
                                     void* trace_data = nullptr);

#endif  // EDGE_ENVIRONMENT_RUNTIME_H_
