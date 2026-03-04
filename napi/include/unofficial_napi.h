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

// Unofficial helper. Enqueues a JS function into V8 microtask queue.
NAPI_EXTERN napi_status unofficial_napi_enqueue_microtask(napi_env env, napi_value callback);

// Unofficial helper. Sets the per-env PromiseReject callback used by
// internal/process/promises via internalBinding('task_queue').
NAPI_EXTERN napi_status unofficial_napi_set_promise_reject_callback(napi_env env,
                                                                    napi_value callback);

// Unofficial helpers used by util/options parity work in ubi.
// These expose engine-specific data that is not available in the public N-API.
NAPI_EXTERN napi_status unofficial_napi_get_promise_details(napi_env env,
                                                            napi_value promise,
                                                            int32_t* state_out,
                                                            napi_value* result_out,
                                                            bool* has_result_out);

NAPI_EXTERN napi_status unofficial_napi_get_proxy_details(napi_env env,
                                                          napi_value proxy,
                                                          napi_value* target_out,
                                                          napi_value* handler_out);

NAPI_EXTERN napi_status unofficial_napi_preview_entries(napi_env env,
                                                        napi_value value,
                                                        napi_value* entries_out,
                                                        bool* is_key_value_out);

NAPI_EXTERN napi_status unofficial_napi_get_call_sites(napi_env env,
                                                       uint32_t frames,
                                                       napi_value* callsites_out);

NAPI_EXTERN napi_status unofficial_napi_arraybuffer_view_has_buffer(napi_env env,
                                                                    napi_value value,
                                                                    bool* result_out);

NAPI_EXTERN napi_status unofficial_napi_get_constructor_name(napi_env env,
                                                             napi_value value,
                                                             napi_value* name_out);

// Unofficial helper. Refreshes V8 date/timezone configuration after TZ changes.
NAPI_EXTERN napi_status unofficial_napi_notify_datetime_configuration_change(napi_env env);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // UNOFFICIAL_NAPI_H_
