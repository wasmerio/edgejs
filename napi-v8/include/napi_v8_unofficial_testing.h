#ifndef NAPI_V8_UNOFFICIAL_TESTING_H_
#define NAPI_V8_UNOFFICIAL_TESTING_H_

#include "js_native_api.h"

#ifdef __cplusplus
extern "C" {
#endif

// Unofficial/test-only helper APIs for creating an env scope
// without exposing direct V8 types in test runner headers.
NAPI_EXTERN napi_status unofficial_napi_v8_open_env_scope(int32_t module_api_version,
                                                          napi_env* env_out,
                                                          void** scope_out);
NAPI_EXTERN napi_status unofficial_napi_v8_close_env_scope(void* scope);
NAPI_EXTERN napi_status unofficial_napi_request_gc_for_testing(napi_env env);

#ifdef __cplusplus
}
#endif

#endif  // NAPI_V8_UNOFFICIAL_TESTING_H_
