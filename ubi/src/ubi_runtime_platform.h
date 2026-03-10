#ifndef UBI_RUNTIME_PLATFORM_H_
#define UBI_RUNTIME_PLATFORM_H_

#include "node_api.h"

using UbiRuntimePlatformTaskCallback = void (*)(napi_env env, void* data);
using UbiRuntimePlatformTaskCleanup = void (*)(napi_env env, void* data);

enum UbiRuntimePlatformTaskFlags : int {
  kUbiRuntimePlatformTaskNone = 0,
  kUbiRuntimePlatformTaskRefed = 1 << 0,
};

// Queue a native immediate/platform task for the current env. Tasks run on the
// owning thread before JS immediates, mirroring Node's native immediate queue.
// Immediate-task APIs are owning-thread-only; cross-thread engine work must use
// the foreground task enqueue hook instead.
napi_status UbiRuntimePlatformEnqueueTask(napi_env env,
                                         UbiRuntimePlatformTaskCallback callback,
                                         void* data,
                                         UbiRuntimePlatformTaskCleanup cleanup,
                                         int flags);

// Drain queued native immediate tasks. Returns the number of tasks run.
size_t UbiRuntimePlatformDrainImmediateTasks(napi_env env, bool only_refed = false);

bool UbiRuntimePlatformHasImmediateTasks(napi_env env);
bool UbiRuntimePlatformHasRefedImmediateTasks(napi_env env);

// Attach the current env to the embedder-owned foreground task queue hook.
// Ubi owns queueing and drain policy; engine backends only post work into it.
napi_status UbiRuntimePlatformInstallHooks(napi_env env);

napi_status UbiRuntimePlatformEnqueueForegroundTask(napi_env env,
                                                    UbiRuntimePlatformTaskCallback callback,
                                                    void* data,
                                                    UbiRuntimePlatformTaskCleanup cleanup,
                                                    uint64_t delay_millis = 0);

// Drain Ubi-owned foreground tasks that were posted by the engine adapter.
napi_status UbiRuntimePlatformDrainTasks(napi_env env);

#endif  // UBI_RUNTIME_PLATFORM_H_
