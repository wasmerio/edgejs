#include "internal/napi_v8_env.h"
#include "node_api.h"
#include "node_api_types.h"

#include <atomic>
#include <cstring>
#include <new>
#include <memory>
#include <vector>

#include <uv.h>

struct napi_async_work__ {
  napi_env env = nullptr;
  napi_async_execute_callback execute = nullptr;
  napi_async_complete_callback complete = nullptr;
  void* data = nullptr;
  uv_work_t req{};
  bool queued = false;
  bool deleting = false;
};

struct napi_threadsafe_function__ {
  napi_env env = nullptr;
  napi_threadsafe_function_call_js call_js_cb = nullptr;
  napi_finalize finalize_cb = nullptr;
  void* finalize_data = nullptr;
  void* context = nullptr;
  std::atomic<uint32_t> refcount{0};
  std::atomic<bool> finalized{false};
};

struct napi_async_cleanup_hook_handle__ {
  napi_env env = nullptr;
  napi_async_cleanup_hook hook = nullptr;
  void* arg = nullptr;
  bool removed = false;
};

struct napi_async_context__ {
  napi_env env = nullptr;
  v8::Global<v8::Object> resource;
};

struct napi_callback_scope__ {
  napi_env env = nullptr;
};

struct napi_buffer_record__ {
  napi_env env = nullptr;
  v8::Global<v8::Object> holder;
  std::shared_ptr<v8::BackingStore> backing_store;
  void* external_data = nullptr;
  node_api_basic_finalize finalize_cb = nullptr;
  void* finalize_hint = nullptr;
  bool finalized = false;
};
using napi_buffer_record = napi_buffer_record__;

struct napi_env_cleanup_hook__ {
  napi_cleanup_hook hook = nullptr;
  void* arg = nullptr;
};

namespace {

inline bool CheckEnv(napi_env env) {
  return env != nullptr && env->isolate != nullptr;
}

void UvExecute(uv_work_t* req) {
  auto* work = static_cast<napi_async_work__*>(req->data);
  if (work == nullptr || work->execute == nullptr) return;
  work->execute(work->env, work->data);
}

void UvAfterWork(uv_work_t* req, int status) {
  auto* work = static_cast<napi_async_work__*>(req->data);
  if (work == nullptr || work->complete == nullptr || work->deleting) return;
  napi_status napi_status_code = (status == UV_ECANCELED) ? napi_cancelled : napi_ok;
  work->complete(work->env, napi_status_code, work->data);
}

void RemoveBufferRecord(napi_env env, napi_buffer_record* record) {
  if (!CheckEnv(env) || record == nullptr) return;
  auto& records = env->buffer_records;
  for (auto it = records.begin(); it != records.end(); ++it) {
    if (*it == record) {
      records.erase(it);
      return;
    }
  }
}

void FinalizeBufferRecord(napi_buffer_record* record) {
  if (record == nullptr || record->finalized) return;
  record->finalized = true;
  if (record->finalize_cb != nullptr) {
    record->finalize_cb(record->env, record->external_data, record->finalize_hint);
  }
}

void BufferWeakCallback(const v8::WeakCallbackInfo<napi_buffer_record>& info) {
  napi_buffer_record* record = info.GetParameter();
  if (record == nullptr) return;
  napi_env env = record->env;
  if (CheckEnv(env)) {
    v8::HandleScope hs(env->isolate);
    v8::Local<v8::Context> context = env->context();
    v8::Context::Scope cs(context);
    RemoveBufferRecord(env, record);
    FinalizeBufferRecord(record);
  }
  record->holder.Reset();
  delete record;
}

bool IsOurBufferObject(napi_env env, v8::Local<v8::Object> object) {
  v8::Local<v8::Private> key = env->buffer_private_key.Get(env->isolate);
  return object->HasPrivate(env->context(), key).FromMaybe(false);
}

napi_status GetBufferRecord(napi_env env, napi_value value, napi_buffer_record** out) {
  if (!CheckEnv(env) || value == nullptr || out == nullptr) return napi_invalid_arg;
  v8::Local<v8::Value> raw = napi_v8_unwrap_value(value);
  if (!raw->IsObject()) return napi_invalid_arg;
  v8::Local<v8::Object> object = raw.As<v8::Object>();
  v8::Local<v8::Private> key = env->buffer_private_key.Get(env->isolate);
  v8::MaybeLocal<v8::Value> maybe = object->GetPrivate(env->context(), key);
  v8::Local<v8::Value> external;
  if (!maybe.ToLocal(&external) || !external->IsExternal()) return napi_invalid_arg;
  *out = static_cast<napi_buffer_record*>(external.As<v8::External>()->Value());
  return (*out == nullptr) ? napi_invalid_arg : napi_ok;
}

v8::Local<v8::Object> CreateBufferObject(napi_env env,
                                         std::shared_ptr<v8::BackingStore> backing_store,
                                         size_t offset,
                                         size_t length) {
  v8::Local<v8::ArrayBuffer> ab = v8::ArrayBuffer::New(env->isolate, backing_store);
  return v8::Uint8Array::New(ab, offset, length);
}

}  // namespace

void napi_v8_run_async_cleanup_hooks(napi_env env) {
  if (!CheckEnv(env)) return;

  std::vector<napi_async_cleanup_hook_handle> pending;
  pending.reserve(env->async_cleanup_hooks.size());
  for (void* raw : env->async_cleanup_hooks) {
    auto* handle = static_cast<napi_async_cleanup_hook_handle>(raw);
    if (handle != nullptr && !handle->removed) {
      pending.push_back(handle);
    }
  }

  for (auto* handle : pending) {
    if (handle != nullptr && !handle->removed && handle->hook != nullptr) {
      handle->hook(handle, handle->arg);
    }
  }

  // Drive libuv callbacks queued by async cleanup hooks until hooks are removed.
  size_t guard = 0;
  while (!env->async_cleanup_hooks.empty() && guard++ < 128) {
    uv_run(uv_default_loop(), UV_RUN_DEFAULT);
    bool any_left = false;
    for (void* raw : env->async_cleanup_hooks) {
      auto* handle = static_cast<napi_async_cleanup_hook_handle>(raw);
      if (handle != nullptr && !handle->removed) {
        any_left = true;
        break;
      }
    }
    if (!any_left) break;
  }

  for (void* raw : env->async_cleanup_hooks) {
    auto* handle = static_cast<napi_async_cleanup_hook_handle>(raw);
    delete handle;
  }
  env->async_cleanup_hooks.clear();
}

void napi_v8_finalize_buffer_records(napi_env env) {
  if (!CheckEnv(env)) return;
  for (void* raw : env->buffer_records) {
    auto* record = static_cast<napi_buffer_record*>(raw);
    if (record != nullptr) {
      FinalizeBufferRecord(record);
      record->holder.Reset();
      delete record;
    }
  }
  env->buffer_records.clear();
}

void napi_v8_run_env_cleanup_hooks(napi_env env) {
  if (!CheckEnv(env)) return;
  for (auto it = env->env_cleanup_hooks.rbegin(); it != env->env_cleanup_hooks.rend(); ++it) {
    auto* entry = static_cast<napi_env_cleanup_hook__*>(*it);
    if (entry != nullptr && entry->hook != nullptr) {
      entry->hook(entry->arg);
    }
  }
  for (void* raw : env->env_cleanup_hooks) {
    delete static_cast<napi_env_cleanup_hook__*>(raw);
  }
  env->env_cleanup_hooks.clear();
}

extern "C" {

napi_status NAPI_CDECL node_api_post_finalizer(node_api_basic_env env,
                                               napi_finalize finalize_cb,
                                               void* finalize_data,
                                               void* finalize_hint) {
  napi_env napiEnv = const_cast<napi_env>(env);
  if (!CheckEnv(napiEnv) || finalize_cb == nullptr) return napi_invalid_arg;
  finalize_cb(napiEnv, finalize_data, finalize_hint);
  return napi_ok;
}

napi_status NAPI_CDECL napi_get_node_version(
    node_api_basic_env env, const napi_node_version** version) {
#ifdef NODE_MAJOR_VERSION
  static const uint32_t kMajor = NODE_MAJOR_VERSION;
  static const uint32_t kMinor = NODE_MINOR_VERSION;
  static const uint32_t kPatch = NODE_PATCH_VERSION;
  static const char* kRelease = NODE_RELEASE;
#else
  static const uint32_t kMajor = 0;
  static const uint32_t kMinor = 0;
  static const uint32_t kPatch = 0;
  static const char* kRelease = "node";
#endif
  static const napi_node_version kVersion = {
      kMajor, kMinor, kPatch, kRelease};
  if (version == nullptr) return napi_invalid_arg;
  auto* napiEnv = const_cast<napi_env>(env);
  if (!CheckEnv(napiEnv)) return napi_invalid_arg;
  *version = &kVersion;
  return napi_ok;
}

napi_status NAPI_CDECL node_api_get_module_file_name(
    node_api_basic_env env, const char** result) {
  static const char kModuleUrl[] = "file:///napi-v8-addon.node";
  if (result == nullptr) return napi_invalid_arg;
  auto* napiEnv = const_cast<napi_env>(env);
  if (!CheckEnv(napiEnv)) return napi_invalid_arg;
  *result = kModuleUrl;
  return napi_ok;
}

napi_status NAPI_CDECL napi_create_async_work(napi_env env,
                                              napi_value async_resource,
                                              napi_value async_resource_name,
                                              napi_async_execute_callback execute,
                                              napi_async_complete_callback complete,
                                              void* data,
                                              napi_async_work* result) {
  (void)async_resource;
  (void)async_resource_name;
  if (!CheckEnv(env) || execute == nullptr || complete == nullptr || result == nullptr) {
    return napi_invalid_arg;
  }
  auto* work = new (std::nothrow) napi_async_work__();
  if (work == nullptr) return napi_generic_failure;
  work->env = env;
  work->execute = execute;
  work->complete = complete;
  work->data = data;
  work->req.data = work;
  *result = work;
  return napi_ok;
}

napi_status NAPI_CDECL napi_delete_async_work(napi_env env, napi_async_work work) {
  if (!CheckEnv(env) || work == nullptr) return napi_invalid_arg;
  work->deleting = true;
  delete work;
  return napi_ok;
}

napi_status NAPI_CDECL napi_queue_async_work(node_api_basic_env env, napi_async_work work) {
  auto* napiEnv = const_cast<napi_env>(env);
  if (!CheckEnv(napiEnv) || work == nullptr) return napi_invalid_arg;
  if (work->queued) return napi_generic_failure;
  int rc = uv_queue_work(uv_default_loop(), &work->req, UvExecute, UvAfterWork);
  if (rc != 0) return napi_generic_failure;
  work->queued = true;
  return napi_ok;
}

napi_status NAPI_CDECL napi_cancel_async_work(node_api_basic_env env, napi_async_work work) {
  auto* napiEnv = const_cast<napi_env>(env);
  if (!CheckEnv(napiEnv) || work == nullptr) return napi_invalid_arg;
  if (!work->queued) return napi_generic_failure;
  int rc = uv_cancel(reinterpret_cast<uv_req_t*>(&work->req));
  return (rc == 0) ? napi_ok : napi_generic_failure;
}

napi_status NAPI_CDECL napi_create_threadsafe_function(
    napi_env env,
    napi_value func,
    napi_value async_resource,
    napi_value async_resource_name,
    size_t max_queue_size,
    size_t initial_thread_count,
    void* thread_finalize_data,
    napi_finalize thread_finalize_cb,
    void* context,
    napi_threadsafe_function_call_js call_js_cb,
    napi_threadsafe_function* result) {
  (void)func;
  (void)async_resource;
  (void)async_resource_name;
  (void)max_queue_size;
  if (!CheckEnv(env) || result == nullptr) return napi_invalid_arg;
  auto* tsfn = new (std::nothrow) napi_threadsafe_function__();
  if (tsfn == nullptr) return napi_generic_failure;
  tsfn->env = env;
  tsfn->call_js_cb = call_js_cb;
  tsfn->finalize_cb = thread_finalize_cb;
  tsfn->finalize_data = thread_finalize_data;
  tsfn->context = context;
  tsfn->refcount.store(static_cast<uint32_t>(initial_thread_count == 0 ? 1 : initial_thread_count));
  env->threadsafe_functions.push_back(tsfn);
  *result = tsfn;
  return napi_ok;
}

napi_status NAPI_CDECL napi_get_threadsafe_function_context(
    napi_threadsafe_function func, void** result) {
  if (func == nullptr || result == nullptr) return napi_invalid_arg;
  *result = func->context;
  return napi_ok;
}

napi_status NAPI_CDECL napi_call_threadsafe_function(
    napi_threadsafe_function func, void* data, napi_threadsafe_function_call_mode is_blocking) {
  (void)data;
  (void)is_blocking;
  if (func == nullptr) return napi_invalid_arg;
  return napi_ok;
}

napi_status NAPI_CDECL napi_acquire_threadsafe_function(napi_threadsafe_function func) {
  if (func == nullptr) return napi_invalid_arg;
  func->refcount.fetch_add(1);
  return napi_ok;
}

napi_status NAPI_CDECL napi_release_threadsafe_function(
    napi_threadsafe_function func, napi_threadsafe_function_release_mode mode) {
  (void)mode;
  if (func == nullptr) return napi_invalid_arg;
  uint32_t current = func->refcount.load();
  if (current > 0) func->refcount.fetch_sub(1);
  return napi_ok;
}

napi_status NAPI_CDECL napi_unref_threadsafe_function(
    node_api_basic_env env, napi_threadsafe_function func) {
  auto* napiEnv = const_cast<napi_env>(env);
  if (!CheckEnv(napiEnv) || func == nullptr) return napi_invalid_arg;
  return napi_ok;
}

napi_status NAPI_CDECL napi_ref_threadsafe_function(
    node_api_basic_env env, napi_threadsafe_function func) {
  auto* napiEnv = const_cast<napi_env>(env);
  if (!CheckEnv(napiEnv) || func == nullptr) return napi_invalid_arg;
  return napi_ok;
}

napi_status NAPI_CDECL napi_add_async_cleanup_hook(
    node_api_basic_env env,
    napi_async_cleanup_hook hook,
    void* arg,
    napi_async_cleanup_hook_handle* remove_handle) {
  auto* napiEnv = const_cast<napi_env>(env);
  if (!CheckEnv(napiEnv) || hook == nullptr) return napi_invalid_arg;

  auto* handle = new (std::nothrow) napi_async_cleanup_hook_handle__();
  if (handle == nullptr) return napi_generic_failure;
  handle->env = napiEnv;
  handle->hook = hook;
  handle->arg = arg;

  napiEnv->async_cleanup_hooks.push_back(handle);
  if (remove_handle != nullptr) *remove_handle = handle;
  return napi_ok;
}

napi_status NAPI_CDECL napi_remove_async_cleanup_hook(
    napi_async_cleanup_hook_handle remove_handle) {
  if (remove_handle == nullptr || remove_handle->env == nullptr) return napi_invalid_arg;
  if (remove_handle->removed) return napi_invalid_arg;
  remove_handle->removed = true;

  auto* env = remove_handle->env;
  auto& hooks = env->async_cleanup_hooks;
  for (auto it = hooks.begin(); it != hooks.end(); ++it) {
    if (*it == remove_handle) {
      hooks.erase(it);
      break;
    }
  }
  delete remove_handle;
  return napi_ok;
}

napi_status NAPI_CDECL napi_get_uv_event_loop(node_api_basic_env env, uv_loop_t** loop) {
  auto* napiEnv = const_cast<napi_env>(env);
  if (!CheckEnv(napiEnv) || loop == nullptr) return napi_invalid_arg;
  *loop = uv_default_loop();
  return napi_ok;
}

napi_status NAPI_CDECL napi_async_init(node_api_basic_env env,
                                       napi_value async_resource,
                                       napi_value async_resource_name,
                                       napi_async_context* result) {
  auto* napiEnv = const_cast<napi_env>(env);
  if (!CheckEnv(napiEnv) || async_resource_name == nullptr || result == nullptr) {
    return napi_invalid_arg;
  }
  v8::Local<v8::Context> context = napiEnv->context();
  v8::Context::Scope context_scope(context);

  auto* asyncContext = new (std::nothrow) napi_async_context__();
  if (asyncContext == nullptr) return napi_generic_failure;
  asyncContext->env = napiEnv;

  v8::Local<v8::Object> resource_obj;
  if (async_resource != nullptr) {
    v8::Local<v8::Value> candidate = napi_v8_unwrap_value(async_resource);
    if (candidate->IsObject()) {
      resource_obj = candidate.As<v8::Object>();
    }
  }
  if (resource_obj.IsEmpty()) {
    resource_obj = v8::Object::New(napiEnv->isolate);
  }
  asyncContext->resource.Reset(napiEnv->isolate, resource_obj);
  *result = asyncContext;
  return napi_ok;
}

napi_status NAPI_CDECL napi_async_destroy(node_api_basic_env env, napi_async_context async_context) {
  auto* napiEnv = const_cast<napi_env>(env);
  if (!CheckEnv(napiEnv) || async_context == nullptr) return napi_invalid_arg;
  delete async_context;
  return napi_ok;
}

napi_status NAPI_CDECL napi_make_callback(napi_env env,
                                         napi_async_context async_context,
                                         napi_value recv,
                                         napi_value func,
                                         size_t argc,
                                         const napi_value* argv,
                                         napi_value* result) {
  (void)async_context;
  return napi_call_function(env, recv, func, argc, argv, result);
}

napi_status NAPI_CDECL napi_fatal_exception(napi_env env, napi_value err) {
  if (!CheckEnv(env) || err == nullptr) return napi_invalid_arg;
  env->last_exception.Reset(env->isolate, napi_v8_unwrap_value(err));
  env->isolate->ThrowException(napi_v8_unwrap_value(err));
  return napi_ok;
}

napi_status NAPI_CDECL napi_add_env_cleanup_hook(node_api_basic_env env,
                                                 napi_cleanup_hook fun,
                                                 void* arg) {
  auto* napiEnv = const_cast<napi_env>(env);
  if (!CheckEnv(napiEnv) || fun == nullptr) return napi_invalid_arg;
  auto* entry = new (std::nothrow) napi_env_cleanup_hook__();
  if (entry == nullptr) return napi_generic_failure;
  entry->hook = fun;
  entry->arg = arg;
  napiEnv->env_cleanup_hooks.push_back(entry);
  return napi_ok;
}

napi_status NAPI_CDECL napi_remove_env_cleanup_hook(node_api_basic_env env,
                                                    napi_cleanup_hook fun,
                                                    void* arg) {
  auto* napiEnv = const_cast<napi_env>(env);
  if (!CheckEnv(napiEnv) || fun == nullptr) return napi_invalid_arg;
  auto& hooks = napiEnv->env_cleanup_hooks;
  for (auto it = hooks.begin(); it != hooks.end(); ++it) {
    auto* entry = static_cast<napi_env_cleanup_hook__*>(*it);
    if (entry != nullptr && entry->hook == fun && entry->arg == arg) {
      delete entry;
      hooks.erase(it);
      return napi_ok;
    }
  }
  return napi_invalid_arg;
}

napi_status NAPI_CDECL napi_open_callback_scope(napi_env env,
                                                napi_value resource_object,
                                                napi_async_context context,
                                                napi_callback_scope* result) {
  (void)resource_object;
  (void)context;
  if (!CheckEnv(env) || result == nullptr) return napi_invalid_arg;
  auto* scope = new (std::nothrow) napi_callback_scope__();
  if (scope == nullptr) return napi_generic_failure;
  scope->env = env;
  *result = scope;
  return napi_ok;
}

napi_status NAPI_CDECL napi_close_callback_scope(napi_env env, napi_callback_scope scope) {
  if (!CheckEnv(env) || scope == nullptr) return napi_invalid_arg;
  delete scope;
  return napi_ok;
}

napi_status NAPI_CDECL napi_create_buffer(napi_env env,
                                          size_t length,
                                          void** data,
                                          napi_value* result) {
  if (!CheckEnv(env) || data == nullptr || result == nullptr) return napi_invalid_arg;
  v8::Local<v8::Context> context = env->context();
  v8::Context::Scope context_scope(context);

  auto backing = v8::ArrayBuffer::NewBackingStore(env->isolate, length);
  if (!backing) return napi_generic_failure;
  *data = backing->Data();

  auto* record = new (std::nothrow) napi_buffer_record__();
  if (record == nullptr) return napi_generic_failure;
  record->env = env;
  record->backing_store = std::move(backing);

  v8::Local<v8::Object> buffer_obj = CreateBufferObject(env, record->backing_store, 0, length);
  record->holder.Reset(env->isolate, buffer_obj);
  record->holder.SetWeak(record, BufferWeakCallback, v8::WeakCallbackType::kParameter);
  v8::Local<v8::Private> key = env->buffer_private_key.Get(env->isolate);
  buffer_obj
      ->SetPrivate(env->context(), key, v8::External::New(env->isolate, record))
      .FromJust();
  env->buffer_records.push_back(record);

  *result = napi_v8_wrap_value(env, buffer_obj);
  return (*result == nullptr) ? napi_generic_failure : napi_ok;
}

napi_status NAPI_CDECL napi_create_buffer_copy(napi_env env,
                                               size_t length,
                                               const void* data,
                                               void** result_data,
                                               napi_value* result) {
  void* out = nullptr;
  napi_status status = napi_create_buffer(env, length, &out, result);
  if (status != napi_ok) return status;
  if (length > 0 && data != nullptr) {
    std::memcpy(out, data, length);
  }
  if (result_data != nullptr) *result_data = out;
  return napi_ok;
}

napi_status NAPI_CDECL napi_create_external_buffer(napi_env env,
                                                   size_t length,
                                                   void* data,
                                                   node_api_basic_finalize finalize_cb,
                                                   void* finalize_hint,
                                                   napi_value* result) {
  if (!CheckEnv(env) || data == nullptr || result == nullptr) return napi_invalid_arg;
  v8::Local<v8::Context> context = env->context();
  v8::Context::Scope context_scope(context);

  auto backing = v8::ArrayBuffer::NewBackingStore(env->isolate, length);
  if (!backing) return napi_generic_failure;
  if (length > 0) {
    std::memcpy(backing->Data(), data, length);
  }

  auto* record = new (std::nothrow) napi_buffer_record__();
  if (record == nullptr) return napi_generic_failure;
  record->env = env;
  record->backing_store = std::move(backing);
  record->external_data = data;
  record->finalize_cb = finalize_cb;
  record->finalize_hint = finalize_hint;

  v8::Local<v8::Object> buffer_obj = CreateBufferObject(env, record->backing_store, 0, length);
  record->holder.Reset(env->isolate, buffer_obj);
  record->holder.SetWeak(record, BufferWeakCallback, v8::WeakCallbackType::kParameter);
  v8::Local<v8::Private> key = env->buffer_private_key.Get(env->isolate);
  buffer_obj
      ->SetPrivate(env->context(), key, v8::External::New(env->isolate, record))
      .FromJust();
  env->buffer_records.push_back(record);

  *result = napi_v8_wrap_value(env, buffer_obj);
  return (*result == nullptr) ? napi_generic_failure : napi_ok;
}

napi_status NAPI_CDECL napi_is_buffer(napi_env env, napi_value value, bool* result) {
  if (!CheckEnv(env) || value == nullptr || result == nullptr) return napi_invalid_arg;
  v8::Local<v8::Value> raw = napi_v8_unwrap_value(value);
  if (!raw->IsObject()) {
    *result = false;
    return napi_ok;
  }
  *result = IsOurBufferObject(env, raw.As<v8::Object>());
  return napi_ok;
}

napi_status NAPI_CDECL napi_get_buffer_info(napi_env env,
                                            napi_value value,
                                            void** data,
                                            size_t* length) {
  if (!CheckEnv(env) || value == nullptr) return napi_invalid_arg;
  napi_buffer_record* record = nullptr;
  napi_status status = GetBufferRecord(env, value, &record);
  if (status != napi_ok) return napi_invalid_arg;
  if (data != nullptr) *data = record->backing_store->Data();
  if (length != nullptr) *length = record->backing_store->ByteLength();
  return napi_ok;
}

napi_status NAPI_CDECL node_api_create_buffer_from_arraybuffer(
    napi_env env,
    napi_value arraybuffer,
    size_t byte_offset,
    size_t byte_length,
    napi_value* result) {
  if (!CheckEnv(env) || arraybuffer == nullptr || result == nullptr) return napi_invalid_arg;
  v8::Local<v8::Value> raw = napi_v8_unwrap_value(arraybuffer);
  if (!raw->IsArrayBuffer()) return napi_invalid_arg;
  v8::Local<v8::ArrayBuffer> ab = raw.As<v8::ArrayBuffer>();
  size_t ab_length = ab->ByteLength();
  if (byte_offset > ab_length || byte_length > (ab_length - byte_offset)) {
    return napi_invalid_arg;
  }

  auto* record = new (std::nothrow) napi_buffer_record__();
  if (record == nullptr) return napi_generic_failure;
  record->env = env;
  record->backing_store = ab->GetBackingStore();

  v8::Local<v8::Object> buffer_obj =
      CreateBufferObject(env, record->backing_store, byte_offset, byte_length);
  record->holder.Reset(env->isolate, buffer_obj);
  record->holder.SetWeak(record, BufferWeakCallback, v8::WeakCallbackType::kParameter);
  v8::Local<v8::Private> key = env->buffer_private_key.Get(env->isolate);
  buffer_obj
      ->SetPrivate(env->context(), key, v8::External::New(env->isolate, record))
      .FromJust();
  env->buffer_records.push_back(record);

  *result = napi_v8_wrap_value(env, buffer_obj);
  return (*result == nullptr) ? napi_generic_failure : napi_ok;
}

}  // extern "C"
