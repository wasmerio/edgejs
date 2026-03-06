#include "ubi_async_wrap.h"

#include <vector>

#include "internal_binding/binding_async_wrap.h"
#include <unordered_map>
#include <unordered_set>

#include "internal_binding/helpers.h"
#include "ubi_module_loader.h"
#include "ubi_runtime.h"
#include "ubi_runtime_platform.h"

namespace {

struct AsyncWrapCache {
  napi_ref binding_ref = nullptr;
  napi_ref async_id_fields_ref = nullptr;
  napi_ref queue_destroy_ref = nullptr;
};

std::unordered_map<napi_env, AsyncWrapCache> g_async_wrap_cache;
std::unordered_set<napi_env> g_async_wrap_cleanup_hooks;

void ResetRef(napi_env env, napi_ref* ref) {
  if (env == nullptr || ref == nullptr || *ref == nullptr) return;
  napi_delete_reference(env, *ref);
  *ref = nullptr;
}

void OnAsyncWrapEnvCleanup(void* data) {
  napi_env env = static_cast<napi_env>(data);
  g_async_wrap_cleanup_hooks.erase(env);
  auto it = g_async_wrap_cache.find(env);
  if (it == g_async_wrap_cache.end()) return;
  ResetRef(env, &it->second.binding_ref);
  ResetRef(env, &it->second.async_id_fields_ref);
  ResetRef(env, &it->second.queue_destroy_ref);
  g_async_wrap_cache.erase(it);
}

void EnsureAsyncWrapCleanupHook(napi_env env) {
  if (env == nullptr) return;
  auto [it, inserted] = g_async_wrap_cleanup_hooks.emplace(env);
  if (!inserted) return;
  if (napi_add_env_cleanup_hook(env, OnAsyncWrapEnvCleanup, env) != napi_ok) {
    g_async_wrap_cleanup_hooks.erase(it);
  }
}

napi_value GetRefValue(napi_env env, napi_ref ref) {
  if (env == nullptr || ref == nullptr) return nullptr;
  napi_value value = nullptr;
  if (napi_get_reference_value(env, ref, &value) != napi_ok || value == nullptr) {
    return nullptr;
  }
  return value;
}

napi_value ResolveInternalBinding(napi_env env, const char* name) {
  if (env == nullptr || name == nullptr) return nullptr;

  napi_value global = internal_binding::GetGlobal(env);
  if (global == nullptr) return nullptr;

  napi_value internal_binding = UbiGetInternalBinding(env);
  if (internal_binding == nullptr) {
    if (napi_get_named_property(env, global, "internalBinding", &internal_binding) != napi_ok ||
        internal_binding == nullptr) {
      return nullptr;
    }
  }

  napi_valuetype type = napi_undefined;
  if (napi_typeof(env, internal_binding, &type) != napi_ok || type != napi_function) {
    return nullptr;
  }

  napi_value binding_name = nullptr;
  if (napi_create_string_utf8(env, name, NAPI_AUTO_LENGTH, &binding_name) != napi_ok ||
      binding_name == nullptr) {
    return nullptr;
  }

  napi_value binding = nullptr;
  napi_value argv[1] = {binding_name};
  if (napi_call_function(env, global, internal_binding, 1, argv, &binding) != napi_ok ||
      binding == nullptr) {
    bool pending = false;
    if (napi_is_exception_pending(env, &pending) == napi_ok && pending) {
      napi_value ignored = nullptr;
      napi_get_and_clear_last_exception(env, &ignored);
    }
    return nullptr;
  }
  return binding;
}

AsyncWrapCache& GetCache(napi_env env) {
  EnsureAsyncWrapCleanupHook(env);
  return g_async_wrap_cache[env];
}

napi_value GetAsyncWrapBinding(napi_env env) {
  AsyncWrapCache& cache = GetCache(env);
  napi_value binding = GetRefValue(env, cache.binding_ref);
  if (binding != nullptr) return binding;

  binding = ResolveInternalBinding(env, "async_wrap");
  if (binding == nullptr) return nullptr;

  if (cache.binding_ref != nullptr) {
    napi_delete_reference(env, cache.binding_ref);
    cache.binding_ref = nullptr;
  }
  napi_create_reference(env, binding, 1, &cache.binding_ref);
  return binding;
}

double* GetAsyncIdFields(napi_env env) {
  AsyncWrapCache& cache = GetCache(env);
  napi_value fields = GetRefValue(env, cache.async_id_fields_ref);
  if (fields == nullptr) {
    napi_value binding = GetAsyncWrapBinding(env);
    if (binding == nullptr) return nullptr;
    if (napi_get_named_property(env, binding, "async_id_fields", &fields) != napi_ok ||
        fields == nullptr) {
      return nullptr;
    }
    if (cache.async_id_fields_ref != nullptr) {
      napi_delete_reference(env, cache.async_id_fields_ref);
      cache.async_id_fields_ref = nullptr;
    }
    napi_create_reference(env, fields, 1, &cache.async_id_fields_ref);
  }

  bool is_typedarray = false;
  if (napi_is_typedarray(env, fields, &is_typedarray) != napi_ok || !is_typedarray) {
    return nullptr;
  }

  napi_typedarray_type type = napi_uint8_array;
  size_t length = 0;
  void* data = nullptr;
  napi_value arraybuffer = nullptr;
  size_t byte_offset = 0;
  if (napi_get_typedarray_info(env,
                               fields,
                               &type,
                               &length,
                               &data,
                               &arraybuffer,
                               &byte_offset) != napi_ok ||
      data == nullptr ||
      type != napi_float64_array ||
      length < 4) {
    return nullptr;
  }

  return static_cast<double*>(data);
}

napi_value GetQueueDestroyFunction(napi_env env) {
  AsyncWrapCache& cache = GetCache(env);
  napi_value fn = GetRefValue(env, cache.queue_destroy_ref);
  if (fn != nullptr) return fn;

  napi_value binding = GetAsyncWrapBinding(env);
  if (binding == nullptr) return nullptr;
  if (napi_get_named_property(env, binding, "queueDestroyAsyncId", &fn) != napi_ok ||
      fn == nullptr) {
    return nullptr;
  }

  napi_valuetype type = napi_undefined;
  if (napi_typeof(env, fn, &type) != napi_ok || type != napi_function) {
    return nullptr;
  }

  if (cache.queue_destroy_ref != nullptr) {
    napi_delete_reference(env, cache.queue_destroy_ref);
    cache.queue_destroy_ref = nullptr;
  }
  napi_create_reference(env, fn, 1, &cache.queue_destroy_ref);
  return fn;
}

void CallQueueDestroyFunction(napi_env env, int64_t async_id) {
  if (env == nullptr || async_id <= 0) return;

  napi_value binding = GetAsyncWrapBinding(env);
  napi_value queue_destroy = GetQueueDestroyFunction(env);
  if (binding == nullptr || queue_destroy == nullptr) return;

  napi_value async_id_value = nullptr;
  if (napi_create_int64(env, async_id, &async_id_value) != napi_ok || async_id_value == nullptr) {
    return;
  }

  napi_value ignored = nullptr;
  napi_value argv[1] = {async_id_value};
  if (napi_call_function(env, binding, queue_destroy, 1, argv, &ignored) != napi_ok) {
    bool pending = false;
    if (napi_is_exception_pending(env, &pending) == napi_ok && pending) {
      napi_value ignored_error = nullptr;
      napi_get_and_clear_last_exception(env, &ignored_error);
    }
  }
}

struct DestroyAsyncTask {
  int64_t async_id = -1;
};

void RunDestroyAsyncTask(napi_env env, void* data) {
  auto* task = static_cast<DestroyAsyncTask*>(data);
  if (task == nullptr) return;
  CallQueueDestroyFunction(env, task->async_id);
}

void CleanupDestroyAsyncTask(napi_env /*env*/, void* data) {
  delete static_cast<DestroyAsyncTask*>(data);
}

}  // namespace

int64_t UbiAsyncWrapNextId(napi_env env) {
  double* fields = GetAsyncIdFields(env);
  if (fields == nullptr) return 1;

  constexpr size_t kAsyncIdCounter = 2;
  fields[kAsyncIdCounter] += 1;
  return static_cast<int64_t>(fields[kAsyncIdCounter]);
}

int64_t UbiAsyncWrapExecutionAsyncId(napi_env env) {
  double* fields = GetAsyncIdFields(env);
  if (fields == nullptr) return 0;

  constexpr size_t kExecutionAsyncId = 0;
  constexpr size_t kDefaultTriggerAsyncId = 3;
  const int64_t execution_async_id = static_cast<int64_t>(fields[kExecutionAsyncId]);
  if (execution_async_id > 0) return execution_async_id;
  const int64_t default_trigger_async_id = static_cast<int64_t>(fields[kDefaultTriggerAsyncId]);
  return default_trigger_async_id > 0 ? default_trigger_async_id : 0;
}

const char* UbiAsyncWrapProviderName(int32_t provider_type) {
  switch (provider_type) {
    case kUbiProviderJsStream:
      return "JSSTREAM";
    case kUbiProviderJsUdpWrap:
      return "JSUDPWRAP";
    case kUbiProviderPipeConnectWrap:
      return "PIPECONNECTWRAP";
    case kUbiProviderPipeServerWrap:
      return "PIPESERVERWRAP";
    case kUbiProviderPipeWrap:
      return "PIPEWRAP";
    case kUbiProviderShutdownWrap:
      return "SHUTDOWNWRAP";
    case kUbiProviderTcpConnectWrap:
      return "TCPCONNECTWRAP";
    case kUbiProviderTcpServerWrap:
      return "TCPSERVERWRAP";
    case kUbiProviderTcpWrap:
      return "TCPWRAP";
    case kUbiProviderTtyWrap:
      return "TTYWRAP";
    case kUbiProviderUdpSendWrap:
      return "UDPSENDWRAP";
    case kUbiProviderUdpWrap:
      return "UDPWRAP";
    case kUbiProviderWriteWrap:
      return "WRITEWRAP";
    default:
      return "NONE";
  }
}

void UbiAsyncWrapEmitInit(napi_env env,
                          int64_t async_id,
                          int32_t provider_type,
                          int64_t trigger_async_id,
                          napi_value resource) {
  if (env == nullptr || async_id <= 0) return;

  napi_value hooks = internal_binding::AsyncWrapGetHooksObject(env);
  if (hooks == nullptr) return;

  napi_value init_fn = nullptr;
  if (napi_get_named_property(env, hooks, "init", &init_fn) != napi_ok || init_fn == nullptr) {
    return;
  }

  napi_valuetype fn_type = napi_undefined;
  if (napi_typeof(env, init_fn, &fn_type) != napi_ok || fn_type != napi_function) return;

  napi_value async_id_v = nullptr;
  napi_value type_v = nullptr;
  napi_value trigger_async_id_v = nullptr;
  napi_value promise_hook_v = nullptr;
  napi_create_int64(env, async_id, &async_id_v);
  napi_create_string_utf8(env, UbiAsyncWrapProviderName(provider_type), NAPI_AUTO_LENGTH, &type_v);
  napi_create_int64(env, trigger_async_id, &trigger_async_id_v);
  napi_get_boolean(env, false, &promise_hook_v);
  napi_value argv[5] = {
      async_id_v,
      type_v,
      trigger_async_id_v,
      resource != nullptr ? resource : internal_binding::Undefined(env),
      promise_hook_v,
  };
  napi_value ignored = nullptr;
  if (napi_call_function(env, hooks, init_fn, 5, argv, &ignored) != napi_ok) {
    bool pending = false;
    if (napi_is_exception_pending(env, &pending) == napi_ok && pending) {
      napi_value ignored_error = nullptr;
      napi_get_and_clear_last_exception(env, &ignored_error);
    }
  }
}

napi_status UbiAsyncWrapMakeCallback(napi_env env,
                                     int64_t async_id,
                                     napi_value resource,
                                     napi_value recv,
                                     napi_value callback,
                                     size_t argc,
                                     napi_value* argv,
                                     napi_value* result,
                                     int flags) {
  if (env == nullptr || recv == nullptr || callback == nullptr) return napi_invalid_arg;
  if (async_id < 0) {
    return UbiMakeCallbackWithFlags(env, recv, callback, argc, argv, result, flags);
  }

  napi_value trampoline = internal_binding::AsyncWrapGetCallbackTrampoline(env);
  if (trampoline == nullptr) {
    return UbiMakeCallbackWithFlags(env, recv, callback, argc, argv, result, flags);
  }

  napi_valuetype trampoline_type = napi_undefined;
  if (napi_typeof(env, trampoline, &trampoline_type) != napi_ok || trampoline_type != napi_function) {
    return UbiMakeCallbackWithFlags(env, recv, callback, argc, argv, result, flags);
  }

  napi_value async_id_v = nullptr;
  napi_create_int64(env, async_id, &async_id_v);
  std::vector<napi_value> trampoline_argv;
  trampoline_argv.reserve(argc + 3);
  trampoline_argv.push_back(async_id_v);
  trampoline_argv.push_back(resource != nullptr ? resource : recv);
  trampoline_argv.push_back(callback);
  for (size_t i = 0; i < argc; ++i) {
    trampoline_argv.push_back(argv != nullptr ? argv[i] : internal_binding::Undefined(env));
  }
  return UbiMakeCallbackWithFlags(
      env, recv, trampoline, trampoline_argv.size(), trampoline_argv.data(), result, flags);
}

void UbiAsyncWrapQueueDestroyId(napi_env env, int64_t async_id) {
  if (env == nullptr || async_id <= 0) return;
  auto* task = new DestroyAsyncTask();
  task->async_id = async_id;
  if (UbiRuntimePlatformEnqueueTask(
          env, RunDestroyAsyncTask, task, CleanupDestroyAsyncTask, kUbiRuntimePlatformTaskNone) != napi_ok) {
    CleanupDestroyAsyncTask(env, task);
  }
}

void UbiAsyncWrapReset(napi_env env, int64_t* async_id) {
  if (async_id == nullptr) return;
  if (*async_id > 0) UbiAsyncWrapQueueDestroyId(env, *async_id);
  *async_id = UbiAsyncWrapNextId(env);
}
