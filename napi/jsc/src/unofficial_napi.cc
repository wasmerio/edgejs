#include "internal/napi_jsc_env.h"

#include <dlfcn.h>
#include <uv.h>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

struct ScopedJSString {
  explicit ScopedJSString(const char* utf8)
      : value(utf8 == nullptr ? JSStringCreateWithUTF8CString("")
                              : JSStringCreateWithUTF8CString(utf8)) {}

  ScopedJSString(const char* utf8, size_t length) : value(Create(utf8, length)) {}

  ~ScopedJSString() {
    if (value != nullptr) JSStringRelease(value);
  }

  ScopedJSString(const ScopedJSString&) = delete;
  ScopedJSString& operator=(const ScopedJSString&) = delete;

  static JSStringRef Create(const char* utf8, size_t length) {
    if (utf8 == nullptr) return JSStringCreateWithUTF8CString("");
    if (length == NAPI_AUTO_LENGTH) return JSStringCreateWithUTF8CString(utf8);
    std::string copy(utf8, length);
    return JSStringCreateWithUTF8CString(copy.c_str());
  }

  JSStringRef value = nullptr;
};

struct UnofficialEnvScope {
  napi_env env = nullptr;
};

struct RefHolder {
  napi_ref ref = nullptr;
};

std::mutex g_unofficial_mu;
std::unordered_map<napi_env, RefHolder> g_prepare_stack_trace_callbacks;
std::unordered_map<napi_env, RefHolder> g_promise_reject_callbacks;
std::unordered_map<napi_env, std::array<RefHolder, 4>> g_promise_hooks;
std::unordered_map<napi_env, RefHolder> g_continuation_data;

void CollectGarbageForTesting(napi_env env) {
  if (env == nullptr) return;
  JSGarbageCollect(env->context);
}

inline JSObjectRef ToJSObjectRef(JSValueRef value) {
  return reinterpret_cast<JSObjectRef>(const_cast<OpaqueJSValue*>(value));
}

bool IsNullOrUndefined(napi_env env, napi_value value) {
  if (env == nullptr || value == nullptr) return true;
  JSValueRef raw = napi_jsc_to_js_value(value);
  return JSValueIsNull(env->context, raw) || JSValueIsUndefined(env->context, raw);
}

bool IsObjectLike(napi_env env, napi_value value) {
  return env != nullptr && value != nullptr &&
         JSValueIsObject(env->context, napi_jsc_to_js_value(value));
}

bool IsFunction(napi_env env, napi_value value) {
  return IsObjectLike(env, value) &&
         JSObjectIsFunction(env->context, ToJSObjectRef(napi_jsc_to_js_value(value)));
}

bool GetGlobal(napi_env env, napi_value* result) {
  if (env == nullptr || result == nullptr) return false;
  *result = napi_jsc_to_napi(JSContextGetGlobalObject(env->context));
  return true;
}

bool GetNamedProperty(napi_env env, napi_value object, const char* name, napi_value* result) {
  return napi_get_named_property(env, object, name, result) == napi_ok && result != nullptr &&
         *result != nullptr;
}

bool CallFunction(napi_env env,
                  napi_value recv,
                  napi_value fn,
                  size_t argc,
                  napi_value* argv,
                  napi_value* result) {
  return napi_call_function(env, recv, fn, argc, argv, result) == napi_ok;
}

bool CreateString(napi_env env, const char* value, napi_value* result) {
  return napi_create_string_utf8(env,
                                 value == nullptr ? "" : value,
                                 NAPI_AUTO_LENGTH,
                                 result) == napi_ok &&
         result != nullptr && *result != nullptr;
}

bool GetBoolean(napi_env env, bool value, napi_value* result) {
  return napi_get_boolean(env, value, result) == napi_ok && result != nullptr &&
         *result != nullptr;
}

std::string ValueToUtf8(napi_env env, napi_value value) {
  JSValueRef exception = nullptr;
  return napi_jsc_value_to_utf8(env, value, &exception);
}

bool EvaluateBoolExpression(napi_env env, const char* source, bool* result_out) {
  if (env == nullptr || source == nullptr || result_out == nullptr) return false;
  *result_out = false;
  JSValueRef exception = nullptr;
  ScopedJSString script(source);
  JSValueRef result = JSEvaluateScript(env->context, script.value, nullptr, nullptr, 0, &exception);
  if (!napi_jsc_check_exception(env, exception) || result == nullptr) return false;
  *result_out = JSValueToBoolean(env->context, result);
  return true;
}

bool DeleteRefIfPresent(napi_env env, napi_ref* ref_ptr) {
  if (env == nullptr || ref_ptr == nullptr || *ref_ptr == nullptr) return true;
  napi_status status = napi_delete_reference(env, *ref_ptr);
  if (status != napi_ok) return false;
  *ref_ptr = nullptr;
  return true;
}

bool StoreOptionalRef(napi_env env, napi_value value, RefHolder* holder) {
  if (env == nullptr || holder == nullptr) return false;
  if (!DeleteRefIfPresent(env, &holder->ref)) return false;
  if (value == nullptr || IsNullOrUndefined(env, value)) return true;
  napi_valuetype type = napi_undefined;
  if (napi_typeof(env, value, &type) != napi_ok || type != napi_function) return false;
  return napi_create_reference(env, value, 1, &holder->ref) == napi_ok;
}

bool StoreAnyRef(napi_env env, napi_value value, RefHolder* holder) {
  if (env == nullptr || holder == nullptr) return false;
  if (!DeleteRefIfPresent(env, &holder->ref)) return false;
  if (value == nullptr || IsNullOrUndefined(env, value)) return true;
  return napi_create_reference(env, value, 1, &holder->ref) == napi_ok;
}

bool CreateProxyBackedEntriesPreview(napi_env env,
                                     napi_value value,
                                     napi_value* entries_out,
                                     bool* is_key_value_out) {
  if (env == nullptr || value == nullptr || entries_out == nullptr || is_key_value_out == nullptr) {
    return false;
  }

  static constexpr const char kHelper[] =
      "(function(value){"
      "  if (value instanceof Map) return [Array.from(value.entries()), true];"
      "  if (value instanceof Set) return [Array.from(value.values()), false];"
      "  if (Array.isArray(value)) return [Array.from(value.entries()), true];"
      "  if (typeof value === 'string') return [Array.from(value.entries()), true];"
      "  if (value && typeof value[Symbol.iterator] === 'function') {"
      "    return [Array.from(value), false];"
      "  }"
      "  return [[], false];"
      "})";

  JSValueRef exception = nullptr;
  ScopedJSString script(kHelper);
  JSValueRef helper_value = JSEvaluateScript(env->context, script.value, nullptr, nullptr, 0, &exception);
  if (!napi_jsc_check_exception(env, exception) || helper_value == nullptr ||
      !JSValueIsObject(env->context, helper_value)) {
    return false;
  }

  JSValueRef argv[1] = {napi_jsc_to_js_value(value)};
  JSValueRef call_result = nullptr;
  if (!napi_jsc_call_function(env,
                              ToJSObjectRef(helper_value),
                              JSContextGetGlobalObject(env->context),
                              1,
                              argv,
                              &call_result) ||
      call_result == nullptr || !JSValueIsObject(env->context, call_result)) {
    return false;
  }

  JSObjectRef pair = ToJSObjectRef(call_result);
  JSValueRef entries_value = JSObjectGetPropertyAtIndex(env->context, pair, 0, &exception);
  if (!napi_jsc_check_exception(env, exception)) return false;
  JSValueRef mode_value = JSObjectGetPropertyAtIndex(env->context, pair, 1, &exception);
  if (!napi_jsc_check_exception(env, exception)) return false;

  *entries_out = napi_jsc_to_napi(entries_value);
  *is_key_value_out = JSValueToBoolean(env->context, mode_value);
  return true;
}

bool BuildOwnNonIndexPropertyList(napi_env env,
                                  napi_value value,
                                  uint32_t filter_bits,
                                  napi_value* result_out) {
  if (env == nullptr || value == nullptr || result_out == nullptr) return false;

  static constexpr const char kHelper[] =
      "(function(value, bits){"
      "  const ONLY_WRITABLE = 1;"
      "  const ONLY_ENUMERABLE = 2;"
      "  const ONLY_CONFIGURABLE = 4;"
      "  const SKIP_STRINGS = 8;"
      "  const SKIP_SYMBOLS = 16;"
      "  const isArrayIndex = (key) => {"
      "    if (typeof key !== 'string') return false;"
      "    if (key === '') return false;"
      "    const n = key >>> 0;"
      "    return String(n) === key && n !== 0xFFFFFFFF;"
      "  };"
      "  return Reflect.ownKeys(value).filter((key) => {"
      "    if (isArrayIndex(key)) return false;"
      "    if ((bits & SKIP_STRINGS) && typeof key === 'string') return false;"
      "    if ((bits & SKIP_SYMBOLS) && typeof key === 'symbol') return false;"
      "    const d = Object.getOwnPropertyDescriptor(value, key);"
      "    if (!d) return false;"
      "    if ((bits & ONLY_WRITABLE) && !d.writable) return false;"
      "    if ((bits & ONLY_ENUMERABLE) && !d.enumerable) return false;"
      "    if ((bits & ONLY_CONFIGURABLE) && !d.configurable) return false;"
      "    return true;"
      "  });"
      "})";

  JSValueRef exception = nullptr;
  ScopedJSString script(kHelper);
  JSValueRef helper_value = JSEvaluateScript(env->context, script.value, nullptr, nullptr, 0, &exception);
  if (!napi_jsc_check_exception(env, exception) || helper_value == nullptr ||
      !JSValueIsObject(env->context, helper_value)) {
    return false;
  }

  JSValueRef argv[2] = {napi_jsc_to_js_value(value),
                        JSValueMakeNumber(env->context, static_cast<double>(filter_bits))};
  JSValueRef call_result = nullptr;
  if (!napi_jsc_call_function(env,
                              ToJSObjectRef(helper_value),
                              JSContextGetGlobalObject(env->context),
                              2,
                              argv,
                              &call_result)) {
    return false;
  }
  *result_out = napi_jsc_to_napi(call_result);
  return true;
}

bool ExtractConstructorName(napi_env env, napi_value value, napi_value* result_out) {
  if (env == nullptr || value == nullptr || result_out == nullptr) return false;
  *result_out = nullptr;
  if (!IsObjectLike(env, value)) return CreateString(env, "", result_out);

  napi_value ctor = nullptr;
  if (!GetNamedProperty(env, value, "constructor", &ctor) || !IsFunction(env, ctor)) {
    return CreateString(env, "", result_out);
  }
  napi_value name = nullptr;
  if (!GetNamedProperty(env, ctor, "name", &name) || name == nullptr) {
    return CreateString(env, "", result_out);
  }
  *result_out = name;
  return true;
}

bool CreateStructuredClone(napi_env env,
                           napi_value value,
                           napi_value transfer_list,
                           napi_value* result_out) {
  if (env == nullptr || value == nullptr || result_out == nullptr) return false;
  napi_value global = nullptr;
  napi_value clone = nullptr;
  if (!GetGlobal(env, &global) || !GetNamedProperty(env, global, "structuredClone", &clone) ||
      !IsFunction(env, clone)) {
    return false;
  }

  napi_value options = nullptr;
  napi_value argv[2] = {value, nullptr};
  size_t argc = 1;
  if (transfer_list != nullptr && !IsNullOrUndefined(env, transfer_list)) {
    if (napi_create_object(env, &options) != napi_ok || options == nullptr ||
        napi_set_named_property(env, options, "transfer", transfer_list) != napi_ok) {
      return false;
    }
    argv[1] = options;
    argc = 2;
  }
  return CallFunction(env, global, clone, argc, argv, result_out);
}

bool InitializeRuntimeCapabilities(napi_env env) {
  if (env == nullptr) return false;

  bool has_bigint = false;
  bool has_typed_arrays = false;
  bool has_deferred_promise = false;
  bool has_shared_arraybuffer = false;
  bool has_sab_backing_store = false;
  bool has_detach_support = false;

  if (!EvaluateBoolExpression(env, "typeof BigInt === 'function'", &has_bigint) ||
      !EvaluateBoolExpression(env,
                              "typeof Uint8Array === 'function' && typeof DataView === 'function'",
                              &has_typed_arrays) ||
      !EvaluateBoolExpression(env,
                              "typeof Promise === 'function' && typeof Promise.resolve === 'function'",
                              &has_deferred_promise) ||
      !EvaluateBoolExpression(env,
                              "typeof SharedArrayBuffer === 'function'",
                              &has_shared_arraybuffer) ||
      !EvaluateBoolExpression(env,
                              "typeof structuredClone === 'function'",
                              &has_detach_support)) {
    return false;
  }

  env->supports_shared_arraybuffer = has_shared_arraybuffer;
  env->detached_arraybuffer_supported = has_detach_support;

  if (has_shared_arraybuffer) {
    napi_value global = nullptr;
    napi_value ctor = nullptr;
    napi_value length = nullptr;
    napi_value sab = nullptr;
    napi_value view_ctor = nullptr;
    napi_value view = nullptr;
    void* bytes = nullptr;
    if (GetGlobal(env, &global) &&
        GetNamedProperty(env, global, "SharedArrayBuffer", &ctor) &&
        IsFunction(env, ctor) &&
        napi_create_uint32(env, 8, &length) == napi_ok &&
        napi_new_instance(env, ctor, 1, &length, &sab) == napi_ok &&
        GetNamedProperty(env, global, "Uint8Array", &view_ctor) &&
        IsFunction(env, view_ctor)) {
      napi_value argv[1] = {sab};
      if (napi_new_instance(env, view_ctor, 1, argv, &view) == napi_ok && view != nullptr) {
        has_sab_backing_store =
            napi_jsc_get_typed_array_bytes(env, ToJSObjectRef(napi_jsc_to_js_value(view)),
                                           &bytes, nullptr, nullptr, nullptr) &&
            bytes != nullptr;
      }
    }
  }

  if (!has_bigint || !has_typed_arrays || !has_deferred_promise) {
    napi_jsc_set_last_error(
        env,
        napi_generic_failure,
        "Configured JavaScriptCore runtime lacks required BigInt, TypedArray/DataView, or Promise support");
    return false;
  }

#if NAPI_JSC_REQUIRE_SHARED_ARRAYBUFFER
  if (!has_shared_arraybuffer || !has_sab_backing_store) {
    napi_jsc_set_last_error(
        env,
        napi_generic_failure,
        "Configured JavaScriptCore runtime lacks SharedArrayBuffer raw backing-store support");
    return false;
  }
#endif
  return true;
}

void CleanupPerEnvRefs(napi_env env) {
  auto cleanup_single = [env](auto* map) {
    auto it = map->find(env);
    if (it == map->end()) return;
    DeleteRefIfPresent(env, &it->second.ref);
    map->erase(it);
  };

  cleanup_single(&g_prepare_stack_trace_callbacks);
  cleanup_single(&g_promise_reject_callbacks);
  cleanup_single(&g_continuation_data);

  auto hooks_it = g_promise_hooks.find(env);
  if (hooks_it != g_promise_hooks.end()) {
    for (auto& holder : hooks_it->second) {
      DeleteRefIfPresent(env, &holder.ref);
    }
    g_promise_hooks.erase(hooks_it);
  }
}

}  // namespace

napi_status napi_jsc_unofficial_unsupported(napi_env env, const char* api_name) {
  std::string message = api_name == nullptr ? "Unsupported unofficial API on JavaScriptCore"
                                            : std::string(api_name) +
                                                  " is unsupported on JavaScriptCore";
  return napi_jsc_set_last_error(env, napi_generic_failure, message.c_str());
}

napi_status napi_jsc_get_interned_private_symbol(napi_env env,
                                                 const char* utf8description,
                                                 size_t length,
                                                 napi_value* result_out) {
  NAPI_JSC_CHECK_ENV(env);
  NAPI_JSC_CHECK_ARG(env, result_out);

  const std::string key = utf8description == nullptr
                              ? std::string()
                              : std::string(utf8description,
                                            length == NAPI_AUTO_LENGTH ? std::strlen(utf8description)
                                                                       : length);
  auto it = env->private_symbols.find(key);
  if (it != env->private_symbols.end() && it->second != nullptr) {
    *result_out = napi_jsc_to_napi(it->second);
    return napi_jsc_clear_last_error(env);
  }

  ScopedJSString description(utf8description, length);
  JSValueRef symbol = JSValueMakeSymbol(env->context, description.value);
  if (symbol == nullptr) {
    return napi_jsc_set_last_error(env, napi_generic_failure, "Failed to create symbol");
  }
  env->Protect(symbol);
  env->private_symbols.emplace(key, symbol);
  *result_out = napi_jsc_to_napi(symbol);
  return napi_jsc_clear_last_error(env);
}

napi_status unofficial_napi_create_env_from_global_context(JSGlobalContextRef context,
                                                           int32_t module_api_version,
                                                           napi_env* result) {
  if (context == nullptr || result == nullptr) return napi_invalid_arg;
  *result = nullptr;

  auto* env = new (std::nothrow) napi_env__(context, module_api_version, false);
  if (env == nullptr) return napi_generic_failure;
  if (!InitializeRuntimeCapabilities(env)) {
    delete env;
    return napi_generic_failure;
  }
  *result = env;
  return napi_jsc_clear_last_error(env);
}

extern "C" {

napi_status NAPI_CDECL unofficial_napi_create_env(int32_t module_api_version,
                                                  napi_env* env_out,
                                                  void** scope_out) {
  return unofficial_napi_create_env_with_options(module_api_version, nullptr, env_out, scope_out);
}

napi_status NAPI_CDECL unofficial_napi_create_env_with_options(
    int32_t module_api_version,
    const unofficial_napi_env_create_options* /*options*/,
    napi_env* env_out,
    void** scope_out) {
  if (env_out == nullptr || scope_out == nullptr) return napi_invalid_arg;
  *env_out = nullptr;
  *scope_out = nullptr;

  napi_jsc_prepare_runtime_for_context_creation();
  JSGlobalContextRef context = JSGlobalContextCreate(nullptr);
  if (context == nullptr) return napi_generic_failure;

  auto* env = new (std::nothrow) napi_env__(context, module_api_version, true);
  JSGlobalContextRelease(context);
  if (env == nullptr) return napi_generic_failure;
  if (!InitializeRuntimeCapabilities(env)) {
    delete env;
    return napi_generic_failure;
  }

  auto* scope = new (std::nothrow) UnofficialEnvScope();
  if (scope == nullptr) {
    delete env;
    return napi_generic_failure;
  }
  scope->env = env;
  *env_out = env;
  *scope_out = scope;
  return napi_jsc_clear_last_error(env);
}

napi_status NAPI_CDECL unofficial_napi_set_edge_environment(napi_env env, void* environment) {
  if (env == nullptr) return napi_invalid_arg;
  env->edge_environment = environment;
  return napi_jsc_clear_last_error(env);
}

void* unofficial_napi_get_edge_environment(napi_env env) {
  return env == nullptr ? nullptr : env->edge_environment;
}

napi_status NAPI_CDECL unofficial_napi_set_env_cleanup_callback(
    napi_env env,
    unofficial_napi_env_cleanup_callback callback,
    void* data) {
  if (env == nullptr) return napi_invalid_arg;
  env->env_cleanup_callback = callback;
  env->env_cleanup_callback_data = data;
  return napi_jsc_clear_last_error(env);
}

napi_status NAPI_CDECL unofficial_napi_set_env_destroy_callback(
    napi_env env,
    unofficial_napi_env_destroy_callback callback,
    void* data) {
  if (env == nullptr) return napi_invalid_arg;
  env->env_destroy_callback = callback;
  env->env_destroy_callback_data = data;
  return napi_jsc_clear_last_error(env);
}

napi_status NAPI_CDECL unofficial_napi_set_context_token_callbacks(
    napi_env env,
    unofficial_napi_context_token_callback assign_callback,
    unofficial_napi_context_token_callback unassign_callback,
    void* data) {
  if (env == nullptr) return napi_invalid_arg;
  env->context_token_assign_callback = assign_callback;
  env->context_token_unassign_callback = unassign_callback;
  env->context_token_callback_data = data;
  return napi_jsc_clear_last_error(env);
}

napi_status NAPI_CDECL unofficial_napi_destroy_env_instance(napi_env env) {
  if (env == nullptr) return napi_invalid_arg;

  if (env->env_cleanup_callback != nullptr) {
    env->env_cleanup_callback(env, env->env_cleanup_callback_data);
  }

  {
    std::lock_guard<std::mutex> lock(g_unofficial_mu);
    CleanupPerEnvRefs(env);
  }
  napi_jsc_contextify_cleanup_env(env);
  delete env;
  return napi_ok;
}

napi_status NAPI_CDECL unofficial_napi_release_env(void* scope_ptr) {
  if (scope_ptr == nullptr) return napi_invalid_arg;
  auto* scope = static_cast<UnofficialEnvScope*>(scope_ptr);
  napi_status status = napi_ok;
  if (scope->env != nullptr) {
    status = unofficial_napi_destroy_env_instance(scope->env);
    scope->env = nullptr;
  }
  delete scope;
  return status;
}

napi_status NAPI_CDECL unofficial_napi_release_env_with_loop(void* scope_ptr,
                                                             struct uv_loop_s* /*loop*/) {
  return unofficial_napi_release_env(scope_ptr);
}

napi_status NAPI_CDECL unofficial_napi_low_memory_notification(napi_env env) {
  if (env == nullptr) return napi_invalid_arg;
  CollectGarbageForTesting(env);
  napi_status status = napi_jsc_sweep_wrapper_finalizers(env);
  if (status != napi_ok) return status;
  return napi_jsc_clear_last_error(env);
}

napi_status NAPI_CDECL unofficial_napi_set_flags_from_string(const char* flags, size_t /*length*/) {
  return flags == nullptr ? napi_invalid_arg : napi_ok;
}

napi_status NAPI_CDECL unofficial_napi_set_prepare_stack_trace_callback(
    napi_env env,
    napi_value callback) {
  if (env == nullptr) return napi_invalid_arg;
  if (callback != nullptr && !IsNullOrUndefined(env, callback) && !IsFunction(env, callback)) {
    return napi_invalid_arg;
  }
  std::lock_guard<std::mutex> lock(g_unofficial_mu);
  if (!StoreOptionalRef(env, callback, &g_prepare_stack_trace_callbacks[env])) {
    g_prepare_stack_trace_callbacks.erase(env);
    return napi_jsc_set_last_error(env, napi_invalid_arg, "A function was expected");
  }
  if (g_prepare_stack_trace_callbacks[env].ref == nullptr) {
    g_prepare_stack_trace_callbacks.erase(env);
  }
  return napi_jsc_clear_last_error(env);
}

napi_status NAPI_CDECL unofficial_napi_request_gc_for_testing(napi_env env) {
  if (env == nullptr) return napi_invalid_arg;
  CollectGarbageForTesting(env);
  napi_status status = napi_jsc_sweep_wrapper_finalizers(env);
  if (status != napi_ok) return status;
  return napi_jsc_clear_last_error(env);
}

napi_status NAPI_CDECL unofficial_napi_process_microtasks(napi_env env) {
  if (env == nullptr) return napi_invalid_arg;
  napi_status status = napi_jsc_sweep_wrapper_finalizers(env);
  if (status != napi_ok) return status;
  return napi_jsc_clear_last_error(env);
}

napi_status NAPI_CDECL unofficial_napi_terminate_execution(napi_env env) {
  return napi_jsc_unofficial_unsupported(env, "unofficial_napi_terminate_execution");
}

napi_status NAPI_CDECL unofficial_napi_cancel_terminate_execution(napi_env env) {
  return env == nullptr ? napi_invalid_arg : napi_jsc_clear_last_error(env);
}

napi_status NAPI_CDECL unofficial_napi_set_pending_exception(napi_env env, napi_value error) {
  if (env == nullptr || error == nullptr) return napi_invalid_arg;
  env->SetLastException(napi_jsc_to_js_value(error));
  return napi_jsc_set_last_error(env, napi_pending_exception, "An exception is pending");
}

napi_status NAPI_CDECL unofficial_napi_request_interrupt(
    napi_env env,
    unofficial_napi_interrupt_callback callback,
    void* data) {
  if (env == nullptr || callback == nullptr) return napi_invalid_arg;
  if (env->thread_id != std::this_thread::get_id()) {
    return napi_jsc_unofficial_unsupported(env, "unofficial_napi_request_interrupt");
  }
  callback(env, data);
  return napi_jsc_clear_last_error(env);
}

napi_status NAPI_CDECL unofficial_napi_set_enqueue_foreground_task_callback(
    napi_env env,
    unofficial_napi_enqueue_foreground_task_callback callback,
    void* target) {
  if (env == nullptr) return napi_invalid_arg;
  env->enqueue_foreground_task_callback = callback;
  env->enqueue_foreground_task_target = target;
  return napi_jsc_clear_last_error(env);
}

napi_status NAPI_CDECL unofficial_napi_enqueue_microtask(napi_env env, napi_value callback) {
  if (env == nullptr || callback == nullptr) return napi_invalid_arg;
  if (!IsFunction(env, callback)) {
    return napi_jsc_set_last_error(env, napi_function_expected, "A function was expected");
  }
  napi_value global = nullptr;
  napi_value queue_microtask = nullptr;
  if (GetGlobal(env, &global) &&
      GetNamedProperty(env, global, "queueMicrotask", &queue_microtask) &&
      IsFunction(env, queue_microtask)) {
    napi_value argv[1] = {callback};
    return CallFunction(env, global, queue_microtask, 1, argv, nullptr)
               ? napi_jsc_clear_last_error(env)
               : env->last_error.error_code;
  }

  napi_value promise_ctor = nullptr;
  napi_value resolve_fn = nullptr;
  napi_value promise = nullptr;
  if (!GetNamedProperty(env, global, "Promise", &promise_ctor) || !IsFunction(env, promise_ctor) ||
      !GetNamedProperty(env, promise_ctor, "resolve", &resolve_fn) || !IsFunction(env, resolve_fn)) {
    return napi_jsc_unofficial_unsupported(env, "unofficial_napi_enqueue_microtask");
  }

  napi_value resolved_value = nullptr;
  if (!CallFunction(env, promise_ctor, resolve_fn, 0, nullptr, &promise) || promise == nullptr) {
    return env->last_error.error_code;
  }
  napi_value then_fn = nullptr;
  if (!GetNamedProperty(env, promise, "then", &then_fn) || !IsFunction(env, then_fn)) {
    return napi_jsc_unofficial_unsupported(env, "unofficial_napi_enqueue_microtask");
  }
  napi_value argv[1] = {callback};
  return CallFunction(env, promise, then_fn, 1, argv, &resolved_value)
             ? napi_jsc_clear_last_error(env)
             : env->last_error.error_code;
}

napi_status NAPI_CDECL unofficial_napi_set_promise_reject_callback(napi_env env,
                                                                    napi_value callback) {
  if (env == nullptr) return napi_invalid_arg;
  if (callback != nullptr && !IsNullOrUndefined(env, callback) && !IsFunction(env, callback)) {
    return napi_invalid_arg;
  }
  std::lock_guard<std::mutex> lock(g_unofficial_mu);
  if (!StoreOptionalRef(env, callback, &g_promise_reject_callbacks[env])) {
    g_promise_reject_callbacks.erase(env);
    return napi_jsc_set_last_error(env, napi_invalid_arg, "A function was expected");
  }
  if (g_promise_reject_callbacks[env].ref == nullptr) {
    g_promise_reject_callbacks.erase(env);
  }
  return napi_jsc_clear_last_error(env);
}

napi_status NAPI_CDECL unofficial_napi_set_promise_hooks(napi_env env,
                                                          napi_value init,
                                                          napi_value before,
                                                          napi_value after,
                                                          napi_value resolve) {
  if (env == nullptr) return napi_invalid_arg;
  std::array<napi_value, 4> hooks = {init, before, after, resolve};
  std::lock_guard<std::mutex> lock(g_unofficial_mu);
  auto& holders = g_promise_hooks[env];
  for (size_t i = 0; i < hooks.size(); ++i) {
    napi_value hook = hooks[i];
    if (hook != nullptr && !IsNullOrUndefined(env, hook) && !IsFunction(env, hook)) {
      return napi_invalid_arg;
    }
    if (!StoreOptionalRef(env, hook, &holders[i])) {
      return napi_jsc_set_last_error(env, napi_invalid_arg, "A function was expected");
    }
  }
  return napi_jsc_clear_last_error(env);
}

napi_status NAPI_CDECL unofficial_napi_set_fatal_error_callbacks(
    napi_env env,
    unofficial_napi_fatal_error_callback /*fatal_callback*/,
    unofficial_napi_oom_error_callback /*oom_callback*/) {
  return env == nullptr ? napi_invalid_arg : napi_jsc_clear_last_error(env);
}

napi_status NAPI_CDECL unofficial_napi_set_near_heap_limit_callback(
    napi_env env,
    unofficial_napi_near_heap_limit_callback /*callback*/,
    void* /*data*/) {
  return env == nullptr ? napi_invalid_arg : napi_jsc_clear_last_error(env);
}

napi_status NAPI_CDECL unofficial_napi_remove_near_heap_limit_callback(
    napi_env env,
    size_t /*heap_limit*/) {
  return env == nullptr ? napi_invalid_arg : napi_jsc_clear_last_error(env);
}

napi_status NAPI_CDECL unofficial_napi_set_stack_limit(napi_env env, void* stack_limit) {
  return (env == nullptr || stack_limit == nullptr) ? napi_invalid_arg : napi_jsc_clear_last_error(env);
}

napi_status NAPI_CDECL unofficial_napi_get_promise_details(napi_env env,
                                                            napi_value /*promise*/,
                                                            int32_t* /*state_out*/,
                                                            napi_value* /*result_out*/,
                                                            bool* /*has_result_out*/) {
  return napi_jsc_unofficial_unsupported(env, "unofficial_napi_get_promise_details");
}

napi_status NAPI_CDECL unofficial_napi_get_error_source_positions(
    napi_env env,
    napi_value /*error*/,
    unofficial_napi_error_source_positions* /*out*/) {
  return napi_jsc_unofficial_unsupported(env, "unofficial_napi_get_error_source_positions");
}

napi_status NAPI_CDECL unofficial_napi_preserve_error_source_message(
    napi_env env,
    napi_value /*error*/) {
  return napi_jsc_unofficial_unsupported(env, "unofficial_napi_preserve_error_source_message");
}

napi_status NAPI_CDECL unofficial_napi_set_source_maps_enabled(napi_env env, bool /*enabled*/) {
  return env == nullptr ? napi_invalid_arg : napi_jsc_clear_last_error(env);
}

napi_status NAPI_CDECL unofficial_napi_set_get_source_map_error_source_callback(
    napi_env env,
    napi_value callback) {
  if (env == nullptr) return napi_invalid_arg;
  if (callback != nullptr && !IsNullOrUndefined(env, callback) && !IsFunction(env, callback)) {
    return napi_invalid_arg;
  }
  return napi_jsc_unofficial_unsupported(
      env, "unofficial_napi_set_get_source_map_error_source_callback");
}

napi_status NAPI_CDECL unofficial_napi_get_error_source_line_for_stderr(
    napi_env env,
    napi_value /*error*/,
    napi_value* /*result_out*/) {
  return napi_jsc_unofficial_unsupported(env, "unofficial_napi_get_error_source_line_for_stderr");
}

napi_status NAPI_CDECL unofficial_napi_get_error_thrown_at(napi_env env,
                                                           napi_value /*error*/,
                                                           napi_value* /*result_out*/) {
  return napi_jsc_unofficial_unsupported(env, "unofficial_napi_get_error_thrown_at");
}

napi_status NAPI_CDECL unofficial_napi_take_preserved_error_formatting(
    napi_env env,
    napi_value /*error*/,
    napi_value* /*source_line_out*/,
    napi_value* /*thrown_at_out*/) {
  return napi_jsc_unofficial_unsupported(env, "unofficial_napi_take_preserved_error_formatting");
}

napi_status NAPI_CDECL unofficial_napi_mark_promise_as_handled(napi_env env,
                                                               napi_value /*promise*/) {
  return env == nullptr ? napi_invalid_arg : napi_jsc_clear_last_error(env);
}

napi_status NAPI_CDECL unofficial_napi_get_proxy_details(napi_env env,
                                                          napi_value /*proxy*/,
                                                          napi_value* /*target_out*/,
                                                          napi_value* /*handler_out*/) {
  return napi_jsc_unofficial_unsupported(env, "unofficial_napi_get_proxy_details");
}

napi_status NAPI_CDECL unofficial_napi_preview_entries(napi_env env,
                                                        napi_value value,
                                                        napi_value* entries_out,
                                                        bool* is_key_value_out) {
  if (env == nullptr || value == nullptr || entries_out == nullptr || is_key_value_out == nullptr) {
    return napi_invalid_arg;
  }
  if (!CreateProxyBackedEntriesPreview(env, value, entries_out, is_key_value_out)) {
    return env->last_error.error_code == napi_ok
               ? napi_jsc_set_last_error(env, napi_generic_failure)
               : env->last_error.error_code;
  }
  return napi_jsc_clear_last_error(env);
}

napi_status NAPI_CDECL unofficial_napi_get_call_sites(napi_env env,
                                                       uint32_t /*frames*/,
                                                       napi_value* /*callsites_out*/) {
  return napi_jsc_unofficial_unsupported(env, "unofficial_napi_get_call_sites");
}

napi_status NAPI_CDECL unofficial_napi_get_current_stack_trace(napi_env env,
                                                                uint32_t /*frames*/,
                                                                napi_value* /*callsites_out*/) {
  return napi_jsc_unofficial_unsupported(env, "unofficial_napi_get_current_stack_trace");
}

napi_status NAPI_CDECL unofficial_napi_get_caller_location(napi_env env,
                                                            napi_value* /*location_out*/) {
  return napi_jsc_unofficial_unsupported(env, "unofficial_napi_get_caller_location");
}

napi_status NAPI_CDECL unofficial_napi_arraybuffer_view_has_buffer(napi_env env,
                                                                    napi_value value,
                                                                    bool* result_out) {
  if (env == nullptr || value == nullptr || result_out == nullptr) return napi_invalid_arg;
  *result_out = false;
  napi_value buffer = nullptr;
  if (!GetNamedProperty(env, value, "buffer", &buffer)) {
    return napi_jsc_clear_last_error(env);
  }
  *result_out = !IsNullOrUndefined(env, buffer);
  return napi_jsc_clear_last_error(env);
}

napi_status NAPI_CDECL unofficial_napi_get_constructor_name(napi_env env,
                                                             napi_value value,
                                                             napi_value* name_out) {
  if (env == nullptr || value == nullptr || name_out == nullptr) return napi_invalid_arg;
  if (!ExtractConstructorName(env, value, name_out)) {
    return env->last_error.error_code == napi_ok
               ? napi_jsc_set_last_error(env, napi_generic_failure)
               : env->last_error.error_code;
  }
  return napi_jsc_clear_last_error(env);
}

napi_status NAPI_CDECL unofficial_napi_get_own_non_index_properties(
    napi_env env,
    napi_value value,
    uint32_t filter_bits,
    napi_value* result_out) {
  if (env == nullptr || value == nullptr || result_out == nullptr) return napi_invalid_arg;
  if (!BuildOwnNonIndexPropertyList(env, value, filter_bits, result_out)) {
    return env->last_error.error_code == napi_ok
               ? napi_jsc_set_last_error(env, napi_generic_failure)
               : env->last_error.error_code;
  }
  return napi_jsc_clear_last_error(env);
}

napi_status NAPI_CDECL unofficial_napi_create_private_symbol(napi_env env,
                                                              const char* utf8description,
                                                              size_t length,
                                                              napi_value* result_out) {
  return napi_jsc_get_interned_private_symbol(env, utf8description, length, result_out);
}

napi_status NAPI_CDECL unofficial_napi_structured_clone(napi_env env,
                                                         napi_value value,
                                                         napi_value* result_out) {
  if (env == nullptr || value == nullptr || result_out == nullptr) return napi_invalid_arg;
  if (!CreateStructuredClone(env, value, nullptr, result_out)) {
    return napi_jsc_unofficial_unsupported(env, "unofficial_napi_structured_clone");
  }
  return napi_jsc_clear_last_error(env);
}

napi_status NAPI_CDECL unofficial_napi_structured_clone_with_transfer(
    napi_env env,
    napi_value value,
    napi_value transfer_list,
    napi_value* result_out) {
  if (env == nullptr || value == nullptr || transfer_list == nullptr || result_out == nullptr) {
    return napi_invalid_arg;
  }
  if (!CreateStructuredClone(env, value, transfer_list, result_out)) {
    return napi_jsc_unofficial_unsupported(env, "unofficial_napi_structured_clone_with_transfer");
  }
  return napi_jsc_clear_last_error(env);
}

napi_status NAPI_CDECL unofficial_napi_serialize_value(napi_env env,
                                                        napi_value /*value*/,
                                                        void** /*payload_out*/) {
  return napi_jsc_unofficial_unsupported(env, "unofficial_napi_serialize_value");
}

napi_status NAPI_CDECL unofficial_napi_deserialize_value(napi_env env,
                                                          void* /*payload*/,
                                                          napi_value* /*result_out*/) {
  return napi_jsc_unofficial_unsupported(env, "unofficial_napi_deserialize_value");
}

void unofficial_napi_release_serialized_value(void* /*payload*/) {}

napi_status NAPI_CDECL unofficial_napi_get_process_memory_info(
    napi_env env,
    double* heap_total_out,
    double* heap_used_out,
    double* external_out,
    double* array_buffers_out) {
  if (env == nullptr || heap_total_out == nullptr || heap_used_out == nullptr ||
      external_out == nullptr || array_buffers_out == nullptr) {
    return napi_invalid_arg;
  }
  const double total = static_cast<double>(uv_get_total_memory());
  const double free = static_cast<double>(uv_get_free_memory());
  *heap_total_out = total;
  *heap_used_out = std::max(0.0, total - free);
  *external_out = 0;
  *array_buffers_out = 0;
  return napi_jsc_clear_last_error(env);
}

napi_status NAPI_CDECL unofficial_napi_get_hash_seed(napi_env env, uint64_t* hash_seed_out) {
  if (env == nullptr || hash_seed_out == nullptr) return napi_invalid_arg;
  *hash_seed_out = env->hash_seed;
  return napi_jsc_clear_last_error(env);
}

napi_status NAPI_CDECL unofficial_napi_get_heap_statistics(
    napi_env env,
    unofficial_napi_heap_statistics* stats_out) {
  if (env == nullptr || stats_out == nullptr) return napi_invalid_arg;
  std::memset(stats_out, 0, sizeof(*stats_out));
  stats_out->total_heap_size = static_cast<uint64_t>(uv_get_total_memory());
  stats_out->used_heap_size =
      static_cast<uint64_t>(std::max(0.0,
                                     static_cast<double>(uv_get_total_memory()) -
                                         static_cast<double>(uv_get_free_memory())));
  return napi_jsc_clear_last_error(env);
}

napi_status NAPI_CDECL unofficial_napi_get_heap_space_count(napi_env env, uint32_t* count_out) {
  if (env == nullptr || count_out == nullptr) return napi_invalid_arg;
  *count_out = 0;
  return napi_jsc_clear_last_error(env);
}

napi_status NAPI_CDECL unofficial_napi_get_heap_space_statistics(
    napi_env env,
    uint32_t /*space_index*/,
    unofficial_napi_heap_space_statistics* stats_out) {
  if (env == nullptr || stats_out == nullptr) return napi_invalid_arg;
  std::memset(stats_out, 0, sizeof(*stats_out));
  return napi_jsc_clear_last_error(env);
}

napi_status NAPI_CDECL unofficial_napi_get_heap_code_statistics(
    napi_env env,
    unofficial_napi_heap_code_statistics* stats_out) {
  if (env == nullptr || stats_out == nullptr) return napi_invalid_arg;
  std::memset(stats_out, 0, sizeof(*stats_out));
  return napi_jsc_clear_last_error(env);
}

napi_status NAPI_CDECL unofficial_napi_start_cpu_profile(
    napi_env env,
    unofficial_napi_cpu_profile_start_result* result_out,
    uint32_t* profile_id_out) {
  if (env == nullptr || result_out == nullptr || profile_id_out == nullptr) return napi_invalid_arg;
  *result_out = unofficial_napi_cpu_profile_start_too_many;
  *profile_id_out = 0;
  return napi_jsc_unofficial_unsupported(env, "unofficial_napi_start_cpu_profile");
}

napi_status NAPI_CDECL unofficial_napi_stop_cpu_profile(
    napi_env env,
    uint32_t /*profile_id*/,
    bool* found_out,
    char** json_out,
    size_t* json_len_out) {
  if (env == nullptr || found_out == nullptr || json_out == nullptr || json_len_out == nullptr) {
    return napi_invalid_arg;
  }
  *found_out = false;
  *json_out = nullptr;
  *json_len_out = 0;
  return napi_jsc_unofficial_unsupported(env, "unofficial_napi_stop_cpu_profile");
}

napi_status NAPI_CDECL unofficial_napi_start_heap_profile(napi_env env, bool* started_out) {
  if (env == nullptr || started_out == nullptr) return napi_invalid_arg;
  *started_out = false;
  return napi_jsc_unofficial_unsupported(env, "unofficial_napi_start_heap_profile");
}

napi_status NAPI_CDECL unofficial_napi_stop_heap_profile(
    napi_env env,
    bool* found_out,
    char** json_out,
    size_t* json_len_out) {
  if (env == nullptr || found_out == nullptr || json_out == nullptr || json_len_out == nullptr) {
    return napi_invalid_arg;
  }
  *found_out = false;
  *json_out = nullptr;
  *json_len_out = 0;
  return napi_jsc_unofficial_unsupported(env, "unofficial_napi_stop_heap_profile");
}

napi_status NAPI_CDECL unofficial_napi_take_heap_snapshot(
    napi_env env,
    const unofficial_napi_heap_snapshot_options* /*options*/,
    char** json_out,
    size_t* json_len_out) {
  if (env == nullptr || json_out == nullptr || json_len_out == nullptr) return napi_invalid_arg;
  *json_out = nullptr;
  *json_len_out = 0;
  return napi_jsc_unofficial_unsupported(env, "unofficial_napi_take_heap_snapshot");
}

void unofficial_napi_free_buffer(void* data) {
  std::free(data);
}

napi_status NAPI_CDECL unofficial_napi_get_continuation_preserved_embedder_data(
    napi_env env,
    napi_value* result_out) {
  if (env == nullptr || result_out == nullptr) return napi_invalid_arg;
  std::lock_guard<std::mutex> lock(g_unofficial_mu);
  auto it = g_continuation_data.find(env);
  if (it == g_continuation_data.end() || it->second.ref == nullptr) {
    return napi_get_undefined(env, result_out);
  }
  return napi_get_reference_value(env, it->second.ref, result_out);
}

napi_status NAPI_CDECL unofficial_napi_set_continuation_preserved_embedder_data(
    napi_env env,
    napi_value value) {
  if (env == nullptr || value == nullptr) return napi_invalid_arg;
  std::lock_guard<std::mutex> lock(g_unofficial_mu);
  if (!StoreAnyRef(env, value, &g_continuation_data[env])) {
    g_continuation_data.erase(env);
    return napi_generic_failure;
  }
  if (g_continuation_data[env].ref == nullptr) {
    g_continuation_data.erase(env);
  }
  return napi_jsc_clear_last_error(env);
}

napi_status NAPI_CDECL unofficial_napi_notify_datetime_configuration_change(napi_env env) {
  return env == nullptr ? napi_invalid_arg : napi_jsc_clear_last_error(env);
}

napi_status NAPI_CDECL unofficial_napi_create_serdes_binding(napi_env env,
                                                              napi_value* /*result_out*/) {
  return napi_jsc_unofficial_unsupported(env, "unofficial_napi_create_serdes_binding");
}

}  // extern "C"
