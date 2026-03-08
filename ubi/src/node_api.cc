#include "node_api.h"
#include "node_api_types.h"
#include "ubi_env_loop.h"
#include "ubi_runtime.h"

#include <atomic>
#include <new>
#include <unordered_map>
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
  bool in_after_work = false;
  bool delete_pending = false;
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

struct napi_callback_scope__ {
  napi_env env = nullptr;
};

void napi_v8_run_async_cleanup_hooks(napi_env env);

namespace {

struct UbiEnvState {
  std::vector<napi_async_cleanup_hook_handle> async_cleanup_hooks;
  bool async_cleanup_hook_registered = false;
  uv_loop_t* loop = nullptr;
  bool loop_cleanup_hook_registered = false;
};

std::unordered_map<napi_env, UbiEnvState> g_ubi_env_state;

inline bool CheckEnv(napi_env env) {
  return env != nullptr;
}

UbiEnvState& GetOrCreateEnvState(napi_env env) {
  return g_ubi_env_state[env];
}

UbiEnvState* GetEnvState(napi_env env) {
  auto it = g_ubi_env_state.find(env);
  if (it == g_ubi_env_state.end()) return nullptr;
  return &it->second;
}

void MaybeEraseEnvState(napi_env env) {
  auto it = g_ubi_env_state.find(env);
  if (it == g_ubi_env_state.end()) return;
  const UbiEnvState& state = it->second;
  if (!state.async_cleanup_hook_registered &&
      state.async_cleanup_hooks.empty() &&
      !state.loop_cleanup_hook_registered &&
      state.loop == nullptr) {
    g_ubi_env_state.erase(it);
  }
}

void CloseEnvLoopHandles(uv_loop_t* loop) {
  if (loop == nullptr) return;
  uv_walk(
      loop,
      [](uv_handle_t* handle, void*) {
        if (handle == nullptr || uv_is_closing(handle) != 0) return;
        uv_close(handle, nullptr);
      },
      nullptr);
}

void DrainAndCloseEnvLoop(uv_loop_t* loop) {
  if (loop == nullptr) return;
  CloseEnvLoopHandles(loop);
  for (size_t guard = 0; guard < 1024; ++guard) {
    if (uv_run(loop, UV_RUN_NOWAIT) == 0) {
      if (uv_loop_close(loop) == 0) return;
    }
    CloseEnvLoopHandles(loop);
  }

  CloseEnvLoopHandles(loop);
  while (uv_run(loop, UV_RUN_NOWAIT) != 0) {
    CloseEnvLoopHandles(loop);
  }
  (void)uv_loop_close(loop);
}

void CleanupEnvLoopOnTeardown(void* arg) {
  auto* env = static_cast<napi_env>(arg);
  if (!CheckEnv(env)) return;
  auto* state = GetEnvState(env);
  if (state == nullptr) return;

  uv_loop_t* loop = state->loop;
  state->loop = nullptr;
  state->loop_cleanup_hook_registered = false;
  if (loop != nullptr) {
    DrainAndCloseEnvLoop(loop);
    delete loop;
  }
  MaybeEraseEnvState(env);
}

void RunAsyncCleanupHooksOnEnvTeardown(void* arg) {
  auto* env = static_cast<napi_env>(arg);
  if (!CheckEnv(env)) return;
  napi_v8_run_async_cleanup_hooks(env);
}

void UvExecute(uv_work_t* req) {
  auto* work = static_cast<napi_async_work__*>(req->data);
  if (work == nullptr || work->execute == nullptr) return;
  work->execute(work->env, work->data);
}

void UvAfterWork(uv_work_t* req, int status) {
  auto* work = static_cast<napi_async_work__*>(req->data);
  if (work == nullptr || work->complete == nullptr || work->deleting) return;
  work->queued = false;
  work->in_after_work = true;
  napi_status napi_status_code = (status == UV_ECANCELED) ? napi_cancelled : napi_ok;
  work->complete(work->env, napi_status_code, work->data);
  work->in_after_work = false;
  if (work->delete_pending) {
    work->deleting = true;
    delete work;
  }
}

}  // namespace

void napi_v8_run_async_cleanup_hooks(napi_env env) {
  if (!CheckEnv(env)) return;

  auto* state = GetEnvState(env);
  if (state == nullptr) return;

  std::vector<napi_async_cleanup_hook_handle> pending;
  pending.reserve(state->async_cleanup_hooks.size());
  for (auto* handle : state->async_cleanup_hooks) {
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
  uv_loop_t* loop = UbiGetExistingEnvLoop(env);
  while (!state->async_cleanup_hooks.empty() && guard++ < 128) {
    if (loop != nullptr) {
      uv_run(loop, UV_RUN_DEFAULT);
    }
    bool any_left = false;
    for (auto* handle : state->async_cleanup_hooks) {
      if (handle != nullptr && !handle->removed) {
        any_left = true;
        break;
      }
    }
    if (!any_left) break;
  }

  for (auto* handle : state->async_cleanup_hooks) {
    delete handle;
  }
  state->async_cleanup_hooks.clear();
  state->async_cleanup_hook_registered = false;
  MaybeEraseEnvState(env);
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
  if (work->in_after_work || work->queued) {
    work->delete_pending = true;
    return napi_ok;
  }
  work->deleting = true;
  delete work;
  return napi_ok;
}

napi_status NAPI_CDECL napi_queue_async_work(node_api_basic_env env, napi_async_work work) {
  auto* napiEnv = const_cast<napi_env>(env);
  if (!CheckEnv(napiEnv) || work == nullptr) return napi_invalid_arg;
  if (work->queued) return napi_generic_failure;
  uv_loop_t* loop = UbiGetEnvLoop(napiEnv);
  if (loop == nullptr) return napi_generic_failure;
  int rc = uv_queue_work(loop, &work->req, UvExecute, UvAfterWork);
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
  (void)UbiEnsureEnvLoop(napiEnv, nullptr);
  auto& state = GetOrCreateEnvState(napiEnv);
  if (!state.async_cleanup_hook_registered) {
    napi_status status =
        napi_add_env_cleanup_hook(napiEnv, RunAsyncCleanupHooksOnEnvTeardown, napiEnv);
    if (status != napi_ok) return status;
    state.async_cleanup_hook_registered = true;
  }

  auto* handle = new (std::nothrow) napi_async_cleanup_hook_handle__();
  if (handle == nullptr) return napi_generic_failure;
  handle->env = napiEnv;
  handle->hook = hook;
  handle->arg = arg;

  state.async_cleanup_hooks.push_back(handle);
  if (remove_handle != nullptr) *remove_handle = handle;
  return napi_ok;
}

napi_status NAPI_CDECL napi_remove_async_cleanup_hook(
    napi_async_cleanup_hook_handle remove_handle) {
  if (remove_handle == nullptr || remove_handle->env == nullptr) return napi_invalid_arg;
  if (remove_handle->removed) return napi_invalid_arg;
  remove_handle->removed = true;

  auto* env = remove_handle->env;
  auto* state = GetEnvState(env);
  if (state == nullptr) return napi_invalid_arg;
  auto& hooks = state->async_cleanup_hooks;
  for (auto it = hooks.begin(); it != hooks.end(); ++it) {
    if (*it == remove_handle) {
      hooks.erase(it);
      break;
    }
  }
  delete remove_handle;
  if (hooks.empty() && state->async_cleanup_hook_registered) {
    napi_remove_env_cleanup_hook(env, RunAsyncCleanupHooksOnEnvTeardown, env);
    state->async_cleanup_hook_registered = false;
    MaybeEraseEnvState(env);
  }
  return napi_ok;
}

napi_status NAPI_CDECL napi_get_uv_event_loop(node_api_basic_env env, uv_loop_t** loop) {
  return UbiEnsureEnvLoop(const_cast<napi_env>(env), loop);
}

napi_status NAPI_CDECL napi_make_callback(napi_env env,
                                         napi_async_context async_context,
                                         napi_value recv,
                                         napi_value func,
                                         size_t argc,
                                         const napi_value* argv,
                                         napi_value* result) {
  (void)async_context;
  return UbiMakeCallback(
      env, recv, func, argc, const_cast<napi_value*>(argv), result);
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

}  // extern "C"

napi_status UbiEnsureEnvLoop(napi_env env, uv_loop_t** loop_out) {
  if (!CheckEnv(env)) return napi_invalid_arg;
  auto& state = GetOrCreateEnvState(env);
  if (!state.loop_cleanup_hook_registered) {
    if (napi_add_env_cleanup_hook(env, CleanupEnvLoopOnTeardown, env) != napi_ok) {
      return napi_generic_failure;
    }
    state.loop_cleanup_hook_registered = true;
  }
  if (state.loop == nullptr) {
    auto* loop = new (std::nothrow) uv_loop_t();
    if (loop == nullptr) return napi_generic_failure;
    if (uv_loop_init(loop) != 0) {
      delete loop;
      return napi_generic_failure;
    }
    state.loop = loop;
  }
  if (loop_out != nullptr) *loop_out = state.loop;
  return napi_ok;
}

uv_loop_t* UbiGetEnvLoop(napi_env env) {
  uv_loop_t* loop = nullptr;
  return UbiEnsureEnvLoop(env, &loop) == napi_ok ? loop : nullptr;
}

uv_loop_t* UbiGetExistingEnvLoop(napi_env env) {
  if (!CheckEnv(env)) return nullptr;
  auto* state = GetEnvState(env);
  return state != nullptr ? state->loop : nullptr;
}
