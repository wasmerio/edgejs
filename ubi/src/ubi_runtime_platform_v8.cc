#include "ubi_runtime_platform.h"

#include <deque>
#include <memory>
#include <unordered_map>
#include <unordered_set>

#include "ubi_runtime.h"
#include "ubi_timers_host.h"
#include "unofficial_napi.h"

namespace {

struct PlatformTask {
  UbiRuntimePlatformTaskCallback callback = nullptr;
  UbiRuntimePlatformTaskCleanup cleanup = nullptr;
  void* data = nullptr;
  bool refed = false;
};

struct PlatformTaskState {
  napi_env env_key = nullptr;
  napi_env env = nullptr;
  std::deque<PlatformTask> immediate_tasks;
  size_t refed_immediate_count = 0;
  bool draining_immediates = false;
  bool cleanup_started = false;
};

std::unordered_map<napi_env, std::unique_ptr<PlatformTaskState>> g_platform_states;
std::unordered_set<napi_env> g_platform_cleanup_hook_registered;

PlatformTaskState* GetState(napi_env env) {
  auto it = g_platform_states.find(env);
  return it == g_platform_states.end() ? nullptr : it->second.get();
}

void CleanupTask(napi_env env, PlatformTask* task) {
  if (task == nullptr) return;
  if (task->cleanup != nullptr) {
    task->cleanup(env, task->data);
  }
}

void OnPlatformEnvCleanup(void* arg) {
  napi_env env = static_cast<napi_env>(arg);
  g_platform_cleanup_hook_registered.erase(env);

  PlatformTaskState* state = GetState(env);
  if (state == nullptr) return;

  state->cleanup_started = true;
  while (!state->immediate_tasks.empty()) {
    PlatformTask task = std::move(state->immediate_tasks.front());
    state->immediate_tasks.pop_front();
    CleanupTask(env, &task);
  }
  state->refed_immediate_count = 0;
  g_platform_states.erase(env);
}

void EnsurePlatformCleanupHook(napi_env env) {
  if (env == nullptr) return;
  auto [it, inserted] = g_platform_cleanup_hook_registered.emplace(env);
  if (!inserted) return;
  if (napi_add_env_cleanup_hook(env, OnPlatformEnvCleanup, env) != napi_ok) {
    g_platform_cleanup_hook_registered.erase(it);
  }
}

PlatformTaskState* GetOrCreateState(napi_env env) {
  if (env == nullptr) return nullptr;
  EnsurePlatformCleanupHook(env);
  auto [it, inserted] = g_platform_states.emplace(env, nullptr);
  if (inserted || it->second == nullptr) {
    auto state = std::make_unique<PlatformTaskState>();
    state->env_key = env;
    state->env = env;
    it->second = std::move(state);
  }
  return it->second.get();
}

bool HasPendingException(napi_env env) {
  bool pending = false;
  return napi_is_exception_pending(env, &pending) == napi_ok && pending;
}

}  // namespace

napi_status UbiRuntimePlatformEnqueueTask(napi_env env,
                                         UbiRuntimePlatformTaskCallback callback,
                                         void* data,
                                         UbiRuntimePlatformTaskCleanup cleanup,
                                         int flags) {
  if (env == nullptr || callback == nullptr) return napi_invalid_arg;

  PlatformTaskState* state = GetOrCreateState(env);
  if (state == nullptr || state->cleanup_started) return napi_generic_failure;

  const bool refed = (flags & kUbiRuntimePlatformTaskRefed) != 0;
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

  UbiEnsureTimersImmediatePump(env);
  if (need_ref) {
    UbiToggleImmediateRefFromNative(env, true);
  }

  return napi_ok;
}

size_t UbiRuntimePlatformDrainImmediateTasks(napi_env env, bool only_refed) {
  PlatformTaskState* state = GetState(env);
  if (state == nullptr || state->cleanup_started || state->draining_immediates) return 0;

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
        (void)UbiHandlePendingExceptionNow(env, &handled);
        if (!HasPendingException(env) && handled) {
          continue;
        }
        state->draining_immediates = false;
        if (state->refed_immediate_count == 0) {
          UbiToggleImmediateRefFromNative(env, false);
        }
        return ran;
      }
    }
  }

  state->draining_immediates = false;
  if (state->refed_immediate_count == 0) {
    UbiToggleImmediateRefFromNative(env, false);
  }
  return ran;
}

bool UbiRuntimePlatformHasImmediateTasks(napi_env env) {
  PlatformTaskState* state = GetState(env);
  return state != nullptr && !state->immediate_tasks.empty();
}

bool UbiRuntimePlatformHasRefedImmediateTasks(napi_env env) {
  PlatformTaskState* state = GetState(env);
  return state != nullptr && state->refed_immediate_count != 0;
}

napi_status UbiRuntimePlatformDrainTasks(napi_env env) {
  if (env == nullptr) return napi_invalid_arg;
  return unofficial_napi_process_microtasks(env);
}
