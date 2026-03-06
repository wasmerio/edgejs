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

// Unofficial helper. Creates the native internalBinding('serdes') object
// containing Serializer and Deserializer constructors.
NAPI_EXTERN napi_status unofficial_napi_create_serdes_binding(napi_env env,
                                                              napi_value* result_out);

// Unofficial helpers for implementing internalBinding('contextify') on embedders.
// These are engine-specific APIs and are not part of the public Node-API.
NAPI_EXTERN napi_status unofficial_napi_contextify_make_context(
    napi_env env,
    napi_value sandbox_or_symbol,
    napi_value name,
    napi_value origin_or_undefined,
    bool allow_code_gen_strings,
    bool allow_code_gen_wasm,
    bool own_microtask_queue,
    napi_value host_defined_option_id,
    napi_value* result_out);

NAPI_EXTERN napi_status unofficial_napi_contextify_run_script(
    napi_env env,
    napi_value sandbox_or_null,
    napi_value source,
    napi_value filename,
    int32_t line_offset,
    int32_t column_offset,
    int64_t timeout,
    bool display_errors,
    bool break_on_sigint,
    bool break_on_first_line,
    napi_value host_defined_option_id,
    napi_value* result_out);

NAPI_EXTERN napi_status unofficial_napi_contextify_dispose_context(
    napi_env env,
    napi_value sandbox_or_context_global);

NAPI_EXTERN napi_status unofficial_napi_contextify_compile_function(
    napi_env env,
    napi_value code,
    napi_value filename,
    int32_t line_offset,
    int32_t column_offset,
    napi_value cached_data_or_undefined,
    bool produce_cached_data,
    napi_value parsing_context_or_undefined,
    napi_value context_extensions_or_undefined,
    napi_value params_or_undefined,
    napi_value host_defined_option_id,
    napi_value* result_out);

NAPI_EXTERN napi_status unofficial_napi_contextify_compile_function_for_cjs_loader(
    napi_env env,
    napi_value code,
    napi_value filename,
    bool is_sea_main,
    bool should_detect_module,
    napi_value* result_out);

NAPI_EXTERN napi_status unofficial_napi_contextify_contains_module_syntax(
    napi_env env,
    napi_value code,
    napi_value filename,
    napi_value resource_name_or_undefined,
    bool cjs_var_in_scope,
    bool* result_out);

NAPI_EXTERN napi_status unofficial_napi_contextify_create_cached_data(
    napi_env env,
    napi_value code,
    napi_value filename,
    int32_t line_offset,
    int32_t column_offset,
    napi_value host_defined_option_id,
    napi_value* cached_data_buffer_out);

// Unofficial helpers for implementing internalBinding('module_wrap') on embedders.
// These keep V8 module objects behind an opaque native handle so bindings stay N-API only.
NAPI_EXTERN napi_status unofficial_napi_module_wrap_create_source_text(
    napi_env env,
    napi_value wrapper,
    napi_value url,
    napi_value context_or_undefined,
    napi_value source,
    int32_t line_offset,
    int32_t column_offset,
    napi_value cached_data_or_id,
    void** handle_out);

NAPI_EXTERN napi_status unofficial_napi_module_wrap_create_synthetic(
    napi_env env,
    napi_value wrapper,
    napi_value url,
    napi_value context_or_undefined,
    napi_value export_names,
    napi_value synthetic_eval_steps,
    void** handle_out);

NAPI_EXTERN napi_status unofficial_napi_module_wrap_destroy(
    napi_env env,
    void* handle);

NAPI_EXTERN napi_status unofficial_napi_module_wrap_get_module_requests(
    napi_env env,
    void* handle,
    napi_value* result_out);

NAPI_EXTERN napi_status unofficial_napi_module_wrap_link(
    napi_env env,
    void* handle,
    size_t count,
    void* const* linked_handles);

NAPI_EXTERN napi_status unofficial_napi_module_wrap_instantiate(
    napi_env env,
    void* handle);

NAPI_EXTERN napi_status unofficial_napi_module_wrap_evaluate(
    napi_env env,
    void* handle,
    int64_t timeout,
    bool break_on_sigint,
    napi_value* result_out);

NAPI_EXTERN napi_status unofficial_napi_module_wrap_evaluate_sync(
    napi_env env,
    void* handle,
    napi_value* result_out);

NAPI_EXTERN napi_status unofficial_napi_module_wrap_get_namespace(
    napi_env env,
    void* handle,
    napi_value* result_out);

NAPI_EXTERN napi_status unofficial_napi_module_wrap_get_status(
    napi_env env,
    void* handle,
    int32_t* status_out);

NAPI_EXTERN napi_status unofficial_napi_module_wrap_get_error(
    napi_env env,
    void* handle,
    napi_value* result_out);

NAPI_EXTERN napi_status unofficial_napi_module_wrap_has_top_level_await(
    napi_env env,
    void* handle,
    bool* result_out);

NAPI_EXTERN napi_status unofficial_napi_module_wrap_has_async_graph(
    napi_env env,
    void* handle,
    bool* result_out);

NAPI_EXTERN napi_status unofficial_napi_module_wrap_set_export(
    napi_env env,
    void* handle,
    napi_value export_name,
    napi_value export_value);

NAPI_EXTERN napi_status unofficial_napi_module_wrap_set_module_source_object(
    napi_env env,
    void* handle,
    napi_value source_object);

NAPI_EXTERN napi_status unofficial_napi_module_wrap_get_module_source_object(
    napi_env env,
    void* handle,
    napi_value* result_out);

NAPI_EXTERN napi_status unofficial_napi_module_wrap_create_cached_data(
    napi_env env,
    void* handle,
    napi_value* result_out);

NAPI_EXTERN napi_status unofficial_napi_module_wrap_set_import_module_dynamically_callback(
    napi_env env,
    napi_value callback);

NAPI_EXTERN napi_status unofficial_napi_module_wrap_set_initialize_import_meta_object_callback(
    napi_env env,
    napi_value callback);

NAPI_EXTERN napi_status unofficial_napi_module_wrap_import_module_dynamically(
    napi_env env,
    size_t argc,
    napi_value* argv,
    napi_value* result_out);

NAPI_EXTERN napi_status unofficial_napi_module_wrap_create_required_module_facade(
    napi_env env,
    void* handle,
    napi_value* result_out);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // UNOFFICIAL_NAPI_H_
