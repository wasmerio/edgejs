#ifndef UBI_TASK_QUEUE_H_
#define UBI_TASK_QUEUE_H_

#include "node_api.h"

napi_value UbiGetOrCreateTaskQueueBinding(napi_env env);
napi_status UbiRunTaskQueueTickCallback(napi_env env, bool* called);
bool UbiGetTaskQueueFlags(napi_env env, bool* has_tick_scheduled, bool* has_rejection_to_warn);

#endif  // UBI_TASK_QUEUE_H_
