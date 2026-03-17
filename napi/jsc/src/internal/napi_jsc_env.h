#ifndef NAPI_JSC_ENV_H_
#define NAPI_JSC_ENV_H_

#include <JavaScriptCore/JavaScript.h>

#include <functional>
#include <list>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "js_native_api.h"
#include "node_api.h"
#include "unofficial_napi.h"

typedef void(NAPI_CDECL* napi_cleanup_hook)(void* arg);

struct napi_ref__;
struct napi_callback_info__;

struct napi_handle_scope__ {
  napi_env env = nullptr;
};

struct napi_escapable_handle_scope__ {
  napi_env env = nullptr;
  bool escaped = false;
};

struct napi_deferred__ {
  napi_env env = nullptr;
  JSObjectRef promise = nullptr;
  JSObjectRef resolve = nullptr;
  JSObjectRef reject = nullptr;
};

struct napi_env_cleanup_hook__ {
  napi_cleanup_hook hook = nullptr;
  void* arg = nullptr;
};

struct napi_env__ {
  struct TypeTagEntry {
    JSValueRef value = nullptr;
    napi_type_tag tag{};
  };

  explicit napi_env__(JSGlobalContextRef context_in,
                      int32_t module_api_version_in,
                      bool owns_context_in);
  ~napi_env__();

  static napi_env Get(JSGlobalContextRef context);

  void SetLastException(JSValueRef exception);
  void ClearLastException();
  void Protect(JSValueRef value);
  void Unprotect(JSValueRef value);
  void InitSymbol(JSValueRef& symbol, const char* description);
  void DeinitSymbol(JSValueRef& symbol);
  void EnqueueDeferredFinalizer(std::function<void()> finalizer);
  void DrainDeferredFinalizers();
  void RunEnvCleanupHooks();

  JSGlobalContextRef context = nullptr;
  napi_extended_error_info last_error{};
  std::string last_error_message;
  JSValueRef last_exception = nullptr;
  int32_t module_api_version = 8;
  bool owns_context = false;
  bool in_gc_finalizer = false;
  bool supports_shared_arraybuffer = false;
  bool detached_arraybuffer_supported = false;
  uint64_t hash_seed = 0;
  void* instance_data = nullptr;
  napi_finalize instance_data_finalize_cb = nullptr;
  void* instance_data_finalize_hint = nullptr;
  void* edge_environment = nullptr;
  std::vector<void*> async_cleanup_hooks;
  std::vector<void*> env_cleanup_hooks;
  std::vector<TypeTagEntry> type_tag_entries;
  std::unordered_map<napi_value, std::uintptr_t> active_ref_values;
  std::unordered_map<std::string, JSValueRef> private_symbols;
  std::list<napi_ref> strong_refs;
  std::vector<std::function<void()>> deferred_finalizers;
  std::vector<void*> tracked_wrapper_infos;
  unofficial_napi_env_cleanup_callback env_cleanup_callback = nullptr;
  void* env_cleanup_callback_data = nullptr;
  unofficial_napi_env_destroy_callback env_destroy_callback = nullptr;
  void* env_destroy_callback_data = nullptr;
  unofficial_napi_context_token_callback context_token_assign_callback = nullptr;
  unofficial_napi_context_token_callback context_token_unassign_callback = nullptr;
  void* context_token_callback_data = nullptr;
  unofficial_napi_enqueue_foreground_task_callback enqueue_foreground_task_callback = nullptr;
  void* enqueue_foreground_task_target = nullptr;
  JSValueRef constructor_info_symbol = nullptr;
  JSValueRef function_info_symbol = nullptr;
  JSValueRef external_info_symbol = nullptr;
  JSValueRef reference_info_symbol = nullptr;
  JSValueRef wrapper_info_symbol = nullptr;
  JSValueRef buffer_marker_symbol = nullptr;
  JSValueRef detached_arraybuffer_symbol = nullptr;
  JSValueRef contextify_context_symbol = nullptr;
  bool draining_deferred_finalizers = false;
  std::thread::id thread_id{std::this_thread::get_id()};
};

napi_status napi_jsc_set_last_error(napi_env env,
                                    napi_status status,
                                    const char* message = nullptr);
napi_status napi_jsc_clear_last_error(napi_env env);
napi_status napi_jsc_set_pending_exception(napi_env env,
                                          JSValueRef exception,
                                          const char* message = nullptr);

inline JSValueRef napi_jsc_to_js_value(napi_value value) {
  return reinterpret_cast<JSValueRef>(value);
}

inline napi_value napi_jsc_to_napi(JSValueRef value) {
  return reinterpret_cast<napi_value>(const_cast<OpaqueJSValue*>(value));
}

inline const JSValueRef* napi_jsc_to_js_values(const napi_value* values) {
  return reinterpret_cast<const JSValueRef*>(values);
}

inline napi_env napi_jsc_env_from_context(JSGlobalContextRef context) {
  return napi_env__::Get(context);
}

bool napi_jsc_can_run_js(napi_env env);
void napi_jsc_prepare_runtime_for_context_creation();
bool napi_jsc_check_exception(napi_env env, JSValueRef exception);
bool napi_jsc_value_is_buffer(napi_env env, napi_value value);
JSValueRef napi_jsc_make_string_utf8(napi_env env, const char* str, size_t length);
JSValueRef napi_jsc_make_string_utf16(napi_env env, const char16_t* str, size_t length);
std::string napi_jsc_value_to_utf8(napi_env env, napi_value value, JSValueRef* exception = nullptr);
bool napi_jsc_get_global_function(napi_env env, const char* name, JSObjectRef* result_out);
bool napi_jsc_call_function(napi_env env,
                            JSObjectRef function,
                            JSObjectRef this_object,
                            size_t argc,
                            const JSValueRef argv[],
                            JSValueRef* result_out);
bool napi_jsc_call_constructor(napi_env env,
                               JSObjectRef constructor,
                               size_t argc,
                               const JSValueRef argv[],
                               JSObjectRef* result_out);
bool napi_jsc_set_named_property(napi_env env,
                                 JSObjectRef object,
                                 const char* name,
                                 JSValueRef value,
                                 JSPropertyAttributes attributes = kJSPropertyAttributeNone);
bool napi_jsc_get_named_property(napi_env env,
                                 JSObjectRef object,
                                 const char* name,
                                 JSValueRef* result_out);
bool napi_jsc_define_property(napi_env env,
                              JSObjectRef object,
                              JSValueRef key,
                              JSValueRef value,
                              JSValueRef getter,
                              JSValueRef setter,
                              napi_property_attributes attributes);
bool napi_jsc_create_uint8_array_from_bytes(napi_env env,
                                            void* data,
                                            size_t byte_length,
                                            JSTypedArrayBytesDeallocator deallocator,
                                            void* deallocator_context,
                                            napi_value* result_out,
                                            napi_value* arraybuffer_out = nullptr);
bool napi_jsc_create_uint8_array_for_buffer(napi_env env,
                                            JSObjectRef arraybuffer,
                                            size_t byte_offset,
                                            size_t byte_length,
                                            napi_value* result_out);
bool napi_jsc_get_typed_array_bytes(napi_env env,
                                    JSObjectRef array,
                                    void** data_out,
                                    size_t* byte_length_out,
                                    size_t* byte_offset_out,
                                    JSObjectRef* buffer_out);
napi_status napi_jsc_unofficial_unsupported(napi_env env, const char* api_name);
napi_status napi_jsc_get_interned_private_symbol(napi_env env,
                                                 const char* utf8description,
                                                 size_t length,
                                                 napi_value* result_out);
void napi_jsc_contextify_cleanup_env(napi_env env);
napi_status napi_jsc_sweep_wrapper_finalizers(napi_env env, bool force = false);
napi_status unofficial_napi_create_env_from_global_context(
    JSGlobalContextRef context,
    int32_t module_api_version,
    napi_env* result);

#define NAPI_JSC_CHECK_ENV(env)                                                \
  do {                                                                         \
    if ((env) == nullptr) return napi_invalid_arg;                             \
  } while (0)

#define NAPI_JSC_CHECK_ARG(env, arg)                                           \
  do {                                                                         \
    if ((arg) == nullptr) return napi_jsc_set_last_error((env),                \
                                                         napi_invalid_arg,      \
                                                         "Invalid argument");   \
  } while (0)

#define NAPI_JSC_RETURN_IF_FALSE(env, cond, status, message)                   \
  do {                                                                         \
    if (!(cond)) return napi_jsc_set_last_error((env), (status), (message));   \
  } while (0)

#define NAPI_JSC_CHECK_CAN_RUN_JS(env)                                         \
  do {                                                                         \
    if (!napi_jsc_can_run_js((env))) {                                         \
      return napi_jsc_set_last_error((env),                                    \
                                     napi_cannot_run_js,                       \
                                     "Cannot run JavaScript in finalizer");    \
    }                                                                          \
  } while (0)

#endif  // NAPI_JSC_ENV_H_
