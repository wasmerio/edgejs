#include "edge_runtime_platform.h"

#include <cassert>
#include <atomic>
#include <chrono>
#include <deque>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>
#include <unordered_map>
#include <unordered_set>

#include <uv.h>

#include "edge_runtime.h"
#include "edge_env_loop.h"
#include "edge_timers_host.h"
#include "unofficial_napi.h"

namespace {

using Clock = std::chrono::steady_clock;

struct PlatformTask {
  EdgeRuntimePlatformTaskCallback callback = nullptr;
  EdgeRuntimePlatformTaskCleanup cleanup = nullptr;
  void* data = nullptr;
  bool refed = false;
};

struct DelayedPlatformTask {
  PlatformTask task;
  uint64_t seq = 0;
  Clock::time_point due;
};

struct DelayedPlatformTaskCompare {
  bool operator()(const DelayedPlatformTask& a, const DelayedPlatformTask& b) const {
    if (a.due != b.due) return a.due > b.due;
    return a.seq > b.seq;
  }
};

struct PlatformTaskState {
  napi_env env_key = nullptr;
  napi_env env = nullptr;
  std::thread::id owning_thread;
  std::mutex foreground_mutex;
  size_t foreground_async_refs = 0;

  std::deque<PlatformTask> immediate_tasks;
  size_t refed_immediate_count = 0;
  bool draining_immediates = false;

  std::deque<PlatformTask> foreground_tasks;
  std::priority_queue<DelayedPlatformTask,
                      std::vector<DelayedPlatformTask>,
                      DelayedPlatformTaskCompare> delayed_foreground_tasks;
  uint64_t next_foreground_seq = 0;
  bool draining_foreground = false;
  bool foreground_async_pending = false;

  uv_async_t foreground_async{};
  uv_timer_t foreground_timer{};
  bool foreground_async_initialized = false;
  bool foreground_timer_initialized = false;
  bool foreground_timer_armed = false;
  Clock::time_point foreground_timer_due{};

  std::atomic<bool> cleanup_started {false};
  uint32_t pending_handle_closes = 0;
};

std::unordered_map<napi_env, std::unique_ptr<PlatformTaskState>> g_platform_states;
std::unordered_set<napi_env> g_platform_cleanup_hook_registered;

size_t DrainForegroundTasksFromState(PlatformTaskState* state,
                                     bool run_checkpoint,
                                     bool clear_async_pending,
                                     napi_status* status_out,
                                     size_t* ran_out);

PlatformTaskState* GetState(napi_env env) {
  auto it = g_platform_states.find(env);
  return it == g_platform_states.end() ? nullptr : it->second.get();
}

void AssertOwningThread(const PlatformTaskState* state, const char* where) {
  if (state == nullptr) return;
  assert(state->owning_thread == std::this_thread::get_id() &&
         "immediate/platform task APIs must run on the owning JS thread");
  (void)where;
}

void CleanupTask(napi_env env, PlatformTask* task) {
  if (task == nullptr) return;
  if (task->cleanup != nullptr) {
    task->cleanup(env, task->data);
  }
}

bool HasPendingException(napi_env env) {
  bool pending = false;
  return napi_is_exception_pending(env, &pending) == napi_ok && pending;
}

void MaybeDestroyState(PlatformTaskState* state) {
  if (state == nullptr || !state->cleanup_started.load(std::memory_order_acquire) ||
      state->pending_handle_closes != 0) {
    return;
  }
  g_platform_states.erase(state->env_key);
}

void OnForegroundHandleClosed(uv_handle_t* handle) {
  auto* state = static_cast<PlatformTaskState*>(handle->data);
  if (state == nullptr) return;

  if (handle == reinterpret_cast<uv_handle_t*>(&state->foreground_async)) {
    state->foreground_async_initialized = false;
  } else if (handle == reinterpret_cast<uv_handle_t*>(&state->foreground_timer)) {
    state->foreground_timer_initialized = false;
  }

  if (state->pending_handle_closes > 0) {
    --state->pending_handle_closes;
  }
  MaybeDestroyState(state);
}

void CloseHandleIfInitialized(PlatformTaskState* state, uv_handle_t* handle, bool* initialized_flag) {
  if (state == nullptr || handle == nullptr || initialized_flag == nullptr || !*initialized_flag) return;
  *initialized_flag = false;
  if (uv_is_closing(handle) != 0) return;
  ++state->pending_handle_closes;
  uv_close(handle, OnForegroundHandleClosed);
}

void RefreshForegroundAsyncRef(PlatformTaskState* state) {
  if (state == nullptr || !state->foreground_async_initialized) return;
  uv_handle_t* handle = reinterpret_cast<uv_handle_t*>(&state->foreground_async);
  if (state->foreground_async_refs == 0) {
    uv_unref(handle);
  } else {
    uv_ref(handle);
  }
}

void MoveDueForegroundTasksLocked(PlatformTaskState* state) {
  const Clock::time_point now = Clock::now();
  while (!state->delayed_foreground_tasks.empty() &&
         state->delayed_foreground_tasks.top().due <= now) {
    DelayedPlatformTask delayed =
        std::move(const_cast<DelayedPlatformTask&>(state->delayed_foreground_tasks.top()));
    state->delayed_foreground_tasks.pop();
    state->foreground_tasks.push_back(std::move(delayed.task));
  }
}

void RefreshForegroundTimerLocked(PlatformTaskState* state) {
  if (state == nullptr || !state->foreground_timer_initialized) return;
  if (uv_is_closing(reinterpret_cast<uv_handle_t*>(&state->foreground_timer)) != 0) return;

  if (state->delayed_foreground_tasks.empty()) {
    uv_timer_stop(&state->foreground_timer);
    state->foreground_timer_armed = false;
    return;
  }

  const Clock::time_point due = state->delayed_foreground_tasks.top().due;
  if (state->foreground_timer_armed && due == state->foreground_timer_due) {
    return;
  }

  Clock::duration remaining = due - Clock::now();
  if (remaining < Clock::duration::zero()) {
    remaining = Clock::duration::zero();
  }
  const auto delay_ms = std::chrono::duration_cast<std::chrono::milliseconds>(remaining).count();
  uv_timer_stop(&state->foreground_timer);
  uv_timer_start(&state->foreground_timer,
                 [](uv_timer_t* handle) {
                   auto* state = static_cast<PlatformTaskState*>(handle->data);
                   if (state == nullptr ||
                       state->cleanup_started.load(std::memory_order_acquire)) {
                     return;
                   }
                   if (handle->loop != nullptr) {
                     uv_stop(handle->loop);
                   }
                 },
                 static_cast<uint64_t>(delay_ms),
                 0);
  state->foreground_timer_due = due;
  state->foreground_timer_armed = true;
}

size_t DrainForegroundTasksFromState(PlatformTaskState* state,
                                     bool run_checkpoint,
                                     bool clear_async_pending,
                                     napi_status* status_out,
                                     size_t* ran_out) {
  if (status_out != nullptr) *status_out = napi_ok;
  if (ran_out != nullptr) *ran_out = 0;
  if (state == nullptr ||
      state->cleanup_started.load(std::memory_order_acquire) ||
      state->draining_foreground) {
    return 0;
  }
  AssertOwningThread(state, "DrainForegroundTasksFromState");

  size_t ran = 0;
  state->draining_foreground = true;

  if (clear_async_pending) {
    std::lock_guard<std::mutex> lock(state->foreground_mutex);
    state->foreground_async_pending = false;
  }

  for (;;) {
    std::deque<PlatformTask> batch;
    {
      std::lock_guard<std::mutex> lock(state->foreground_mutex);
      MoveDueForegroundTasksLocked(state);
      RefreshForegroundTimerLocked(state);
      batch.swap(state->foreground_tasks);
    }
    if (batch.empty()) break;

    while (!batch.empty()) {
      PlatformTask task = std::move(batch.front());
      batch.pop_front();
      if (task.callback == nullptr) {
        CleanupTask(state->env, &task);
        continue;
      }
      task.callback(state->env, task.data);
      ++ran;
      CleanupTask(state->env, &task);

      if (HasPendingException(state->env)) {
        bool handled = false;
        (void)EdgeHandlePendingExceptionNow(state->env, &handled);
        if (HasPendingException(state->env)) {
          state->draining_foreground = false;
          if (status_out != nullptr) *status_out = napi_pending_exception;
          if (ran_out != nullptr) *ran_out = ran;
          return ran;
        }
      }
    }

    if (run_checkpoint) {
      napi_status checkpoint_status = EdgeRunCallbackScopeCheckpoint(state->env);
      if (checkpoint_status != napi_ok) {
        state->draining_foreground = false;
        if (status_out != nullptr) *status_out = checkpoint_status;
        if (ran_out != nullptr) *ran_out = ran;
        return ran;
      }
    }
  }

  state->draining_foreground = false;
  if (ran_out != nullptr) *ran_out = ran;
  return ran;
}

bool EnsureForegroundHandles(PlatformTaskState* state) {
  if (state == nullptr) return false;
  uv_loop_t* loop = EdgeGetEnvLoop(state->env);
  if (loop == nullptr) return false;

  if (!state->foreground_async_initialized) {
    state->foreground_async.data = state;
    if (uv_async_init(loop,
                      &state->foreground_async,
                      [](uv_async_t* handle) {
                        auto* state = static_cast<PlatformTaskState*>(handle->data);
                        if (state == nullptr ||
                            state->cleanup_started.load(std::memory_order_acquire) ||
                            state->draining_foreground) {
                          return;
                        }
                        if (handle->loop != nullptr) {
                          uv_stop(handle->loop);
                        }
                      }) != 0) {
      return false;
    }
    state->foreground_async_initialized = true;
    RefreshForegroundAsyncRef(state);
  }

  if (!state->foreground_timer_initialized) {
    state->foreground_timer.data = state;
    if (uv_timer_init(loop, &state->foreground_timer) != 0) {
      return false;
    }
    uv_unref(reinterpret_cast<uv_handle_t*>(&state->foreground_timer));
    state->foreground_timer_initialized = true;
  }

  return true;
}

napi_status EnqueueForegroundTaskFromEngine(void* target,
                                           unofficial_napi_foreground_task_callback callback,
                                           void* data,
                                           unofficial_napi_foreground_task_cleanup cleanup,
                                           uint64_t delay_millis);
void EnsurePlatformCleanupHook(napi_env env);

napi_status InstallForegroundEnqueueHook(napi_env env) {
  if (env == nullptr) return napi_invalid_arg;
  PlatformTaskState* state = nullptr;
  auto [it, inserted] = g_platform_states.emplace(env, nullptr);
  if (inserted || it->second == nullptr) {
    auto created = std::make_unique<PlatformTaskState>();
    created->env_key = env;
    created->env = env;
    created->owning_thread = std::this_thread::get_id();
    it->second = std::move(created);
  }
  state = it->second.get();
  if (state == nullptr || state->cleanup_started.load(std::memory_order_acquire)) {
    return napi_generic_failure;
  }
  if (state == nullptr || !EnsureForegroundHandles(state)) return napi_generic_failure;
  EnsurePlatformCleanupHook(env);
  return unofficial_napi_set_enqueue_foreground_task_callback(
      env, EnqueueForegroundTaskFromEngine, state);
}

void OnPlatformEnvCleanup(void* arg) {
  napi_env env = static_cast<napi_env>(arg);
  g_platform_cleanup_hook_registered.erase(env);

  PlatformTaskState* state = GetState(env);
  if (state == nullptr) return;
  AssertOwningThread(state, "OnPlatformEnvCleanup");

  state->cleanup_started.store(true, std::memory_order_release);
  (void)unofficial_napi_set_enqueue_foreground_task_callback(env, nullptr, nullptr);
  state->foreground_async_refs = 0;
  while (!state->immediate_tasks.empty()) {
    PlatformTask task = std::move(state->immediate_tasks.front());
    state->immediate_tasks.pop_front();
    CleanupTask(env, &task);
  }
  {
    std::lock_guard<std::mutex> lock(state->foreground_mutex);
    state->foreground_async_pending = false;
    while (!state->foreground_tasks.empty()) {
      PlatformTask task = std::move(state->foreground_tasks.front());
      state->foreground_tasks.pop_front();
      CleanupTask(env, &task);
    }
    while (!state->delayed_foreground_tasks.empty()) {
      DelayedPlatformTask delayed =
          std::move(const_cast<DelayedPlatformTask&>(state->delayed_foreground_tasks.top()));
      state->delayed_foreground_tasks.pop();
      CleanupTask(env, &delayed.task);
    }
  }
  state->refed_immediate_count = 0;

  if (state->foreground_timer_initialized) {
    uv_timer_stop(&state->foreground_timer);
    state->foreground_timer_armed = false;
  }

  CloseHandleIfInitialized(state,
                           reinterpret_cast<uv_handle_t*>(&state->foreground_async),
                           &state->foreground_async_initialized);
  CloseHandleIfInitialized(state,
                           reinterpret_cast<uv_handle_t*>(&state->foreground_timer),
                           &state->foreground_timer_initialized);

  MaybeDestroyState(state);
}

void EnsurePlatformCleanupHook(napi_env env) {
  if (env == nullptr) return;
  auto [it, inserted] = g_platform_cleanup_hook_registered.emplace(env);
  if (!inserted) return;
  if (napi_add_env_cleanup_hook(env, OnPlatformEnvCleanup, env) != napi_ok) {
    g_platform_cleanup_hook_registered.erase(it);
  }
}

size_t DrainForegroundTasks(napi_env env, bool run_checkpoint, napi_status* status_out) {
  PlatformTaskState* state = GetState(env);
  return DrainForegroundTasksFromState(state, run_checkpoint, false, status_out, nullptr);
}

napi_status EnqueueForegroundTaskFromEngine(void* target,
                                           unofficial_napi_foreground_task_callback callback,
                                           void* data,
                                           unofficial_napi_foreground_task_cleanup cleanup,
                                           uint64_t delay_millis) {
  auto* state = static_cast<PlatformTaskState*>(target);
  if (state == nullptr || callback == nullptr) return napi_invalid_arg;
  if (state->cleanup_started.load(std::memory_order_acquire)) {
    if (cleanup != nullptr) cleanup(state->env, data);
    return napi_generic_failure;
  }

  PlatformTask task;
  task.callback = callback;
  task.cleanup = cleanup;
  task.data = data;

  bool should_signal = false;
  int signal_rc = 0;
  {
    std::lock_guard<std::mutex> lock(state->foreground_mutex);
    if (state->cleanup_started.load(std::memory_order_acquire) ||
        !state->foreground_async_initialized) {
      CleanupTask(state->env, &task);
      return napi_generic_failure;
    }

    if (delay_millis == 0) {
      state->foreground_tasks.push_back(std::move(task));
    } else {
      DelayedPlatformTask delayed;
      delayed.task = std::move(task);
      delayed.seq = state->next_foreground_seq++;
      delayed.due = Clock::now() + std::chrono::milliseconds(delay_millis);
      state->delayed_foreground_tasks.push(std::move(delayed));
    }

    state->foreground_async_pending = true;
    should_signal = true;

    if (should_signal) {
      signal_rc = uv_async_send(&state->foreground_async);
      if (signal_rc != 0) {
        state->foreground_async_pending = false;
      }
    }
  }

  if (should_signal && signal_rc != 0) {
    return napi_generic_failure;
  }
  return napi_ok;
}

}  // namespace

napi_status EdgeRuntimePlatformInstallHooks(napi_env env) {
  return InstallForegroundEnqueueHook(env);
}

napi_status EdgeRuntimePlatformEnqueueForegroundTask(napi_env env,
                                                   EdgeRuntimePlatformTaskCallback callback,
                                                   void* data,
                                                   EdgeRuntimePlatformTaskCleanup cleanup,
                                                   uint64_t delay_millis) {
  if (env == nullptr || callback == nullptr) return napi_invalid_arg;
  PlatformTaskState* state = GetState(env);
  if (state == nullptr || state->cleanup_started.load(std::memory_order_acquire)) {
    return napi_generic_failure;
  }
  return EnqueueForegroundTaskFromEngine(
      state, callback, data, cleanup, delay_millis);
}

napi_status EdgeRuntimePlatformAddRef(napi_env env) {
  if (env == nullptr) return napi_invalid_arg;
  PlatformTaskState* state = GetState(env);
  if (state == nullptr || state->cleanup_started.load(std::memory_order_acquire)) {
    return napi_generic_failure;
  }
  AssertOwningThread(state, "EdgeRuntimePlatformAddRef");
  if (!state->foreground_async_initialized && !EnsureForegroundHandles(state)) {
    return napi_generic_failure;
  }
  state->foreground_async_refs++;
  RefreshForegroundAsyncRef(state);
  return napi_ok;
}

napi_status EdgeRuntimePlatformReleaseRef(napi_env env) {
  if (env == nullptr) return napi_invalid_arg;
  PlatformTaskState* state = GetState(env);
  if (state == nullptr || state->cleanup_started.load(std::memory_order_acquire)) {
    return napi_generic_failure;
  }
  AssertOwningThread(state, "EdgeRuntimePlatformReleaseRef");
  if (state->foreground_async_refs == 0) {
    return napi_generic_failure;
  }
  state->foreground_async_refs--;
  RefreshForegroundAsyncRef(state);
  return napi_ok;
}

napi_status EdgeRuntimePlatformEnqueueTask(napi_env env,
                                         EdgeRuntimePlatformTaskCallback callback,
                                         void* data,
                                         EdgeRuntimePlatformTaskCleanup cleanup,
                                         int flags) {
  if (env == nullptr || callback == nullptr) return napi_invalid_arg;
  PlatformTaskState* state = GetState(env);
  if (state == nullptr || state->cleanup_started.load(std::memory_order_acquire)) {
    return napi_generic_failure;
  }
  AssertOwningThread(state, "EdgeRuntimePlatformEnqueueTask");

  const bool refed = (flags & kEdgeRuntimePlatformTaskRefed) != 0;
  const bool need_ref = refed && state->refed_immediate_count == 0;

  PlatformTask task;
  task.callback = callback;
  task.cleanup = cleanup;
  task.data = data;
  task.refed = refed;
  state->immediate_tasks.push_back(std::move(task));
  if (refed) {
    state->refed_immediate_count++;
  }

  EdgeEnsureTimersImmediatePump(env);
  if (need_ref) {
    EdgeToggleImmediateRefFromNative(env, true);
  }

  return napi_ok;
}

size_t EdgeRuntimePlatformDrainImmediateTasks(napi_env env, bool only_refed) {
  PlatformTaskState* state = GetState(env);
  if (state == nullptr ||
      state->cleanup_started.load(std::memory_order_acquire) ||
      state->draining_immediates) {
    return 0;
  }
  AssertOwningThread(state, "EdgeRuntimePlatformDrainImmediateTasks");

  size_t ran = 0;
  state->draining_immediates = true;

  for (;;) {
    std::deque<PlatformTask> batch;
    const size_t batch_size = state->immediate_tasks.size();
    for (size_t i = 0; i < batch_size; ++i) {
      PlatformTask task = std::move(state->immediate_tasks.front());
      state->immediate_tasks.pop_front();
      if (only_refed && !task.refed) {
        state->immediate_tasks.push_back(std::move(task));
        continue;
      }
      if (task.refed && state->refed_immediate_count > 0) {
        state->refed_immediate_count--;
      }
      batch.push_back(std::move(task));
    }

    if (batch.empty()) break;

    while (!batch.empty()) {
      PlatformTask task = std::move(batch.front());
      batch.pop_front();

      task.callback(env, task.data);
      ran++;
      CleanupTask(env, &task);

      if (HasPendingException(env)) {
        bool handled = false;
        (void)EdgeHandlePendingExceptionNow(env, &handled);
        if (!HasPendingException(env) && handled) {
          continue;
        }
        state->draining_immediates = false;
        if (state->refed_immediate_count == 0) {
          EdgeToggleImmediateRefFromNative(env, false);
        }
        return ran;
      }
    }
  }

  state->draining_immediates = false;
  if (state->refed_immediate_count == 0) {
    EdgeToggleImmediateRefFromNative(env, false);
  }
  return ran;
}

bool EdgeRuntimePlatformHasImmediateTasks(napi_env env) {
  PlatformTaskState* state = GetState(env);
  AssertOwningThread(state, "EdgeRuntimePlatformHasImmediateTasks");
  return state != nullptr && !state->immediate_tasks.empty();
}

bool EdgeRuntimePlatformHasRefedImmediateTasks(napi_env env) {
  PlatformTaskState* state = GetState(env);
  AssertOwningThread(state, "EdgeRuntimePlatformHasRefedImmediateTasks");
  return state != nullptr && state->refed_immediate_count != 0;
}

napi_status EdgeRuntimePlatformDrainTasks(napi_env env) {
  if (env == nullptr) return napi_invalid_arg;
  PlatformTaskState* state = GetState(env);
  if (state == nullptr || state->cleanup_started.load(std::memory_order_acquire)) {
    return napi_generic_failure;
  }
  napi_status status = napi_ok;
  (void)DrainForegroundTasksFromState(state, true, true, &status, nullptr);
  return status;
}

void EdgeRunRuntimePlatformEnvCleanup(napi_env env) {
  if (env == nullptr) return;
  if (g_platform_cleanup_hook_registered.erase(env) != 0) {
    (void)napi_remove_env_cleanup_hook(env, OnPlatformEnvCleanup, env);
  }
  OnPlatformEnvCleanup(env);
  uv_loop_t* loop = EdgeGetExistingEnvLoop(env);
  PlatformTaskState* state = GetState(env);
  for (size_t guard = 0; loop != nullptr && state != nullptr && state->pending_handle_closes != 0 && guard < 1024;
       ++guard) {
    (void)uv_run(loop, UV_RUN_NOWAIT);
    state = GetState(env);
  }
}
