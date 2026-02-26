#ifndef UNOFFICIAL_NAPI_H_
#define UNOFFICIAL_NAPI_H_

#include "js_native_api.h"

#ifdef __cplusplus
extern "C" {
#endif

// Unofficial/test-only helper APIs for creating and releasing an env scope.
NAPI_EXTERN napi_status unofficial_napi_create_env(int32_t module_api_version,
                                                   napi_env* env_out,
                                                   void** scope_out);
NAPI_EXTERN napi_status unofficial_napi_release_env(void* scope);

// Unofficial/test-only helper. Requests a full GC cycle for testing.
NAPI_EXTERN napi_status unofficial_napi_request_gc_for_testing(napi_env env);

// Unofficial/test-only helper. Processes pending microtasks.
NAPI_EXTERN napi_status unofficial_napi_process_microtasks(napi_env env);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // UNOFFICIAL_NAPI_H_
