#include "edge_timers_host.h"

#include <cstdarg>
#include <cmath>
#include <cstdio>
#include <cstdlib>

#include <uv.h>

#include "edge_environment.h"
#include "edge_env_loop.h"
#include "edge_runtime.h"
#include "edge_runtime_platform.h"
#include "edge_worker_env.h"

namespace {

struct TimersHostState;
void DeleteRefIfAny(napi_env env, napi_ref* ref_slot);
bool GetProcessReceiver(napi_env env, napi_value* recv_out);
bool RunTimersCallbackCheckpoint(TimersHostState* st);
bool CanCallTimersCallback(TimersHostState* st);

struct TimersHostState {
  explicit TimersHostState(napi_env env_in) : env(env_in) {}
  ~TimersHostState() {
    DeleteRefIfAny(env, &timers_callback_ref);
    DeleteRefIfAny(env, &immediate_callback_ref);
    DeleteRefIfAny(env, &immediate_info_ref);
    DeleteRefIfAny(env, &timeout_info_ref);
  }

  napi_env env = nullptr;
  napi_ref timers_callback_ref = nullptr;
  napi_ref immediate_callback_ref = nullptr;
  uv_timer_t timer_handle{};
  uv_check_t check_handle{};
  uv_idle_t idle_handle{};
  napi_ref immediate_info_ref = nullptr;
  napi_ref timeout_info_ref = nullptr;
  double timer_base_ms = -1;
  bool timer_initialized = false;
  bool check_initialized = false;
  bool check_running = false;
  bool idle_initialized = false;
  bool idle_running = false;
  bool running_timers_callback = false;
  bool cleanup_started = false;
  uint32_t pending_handle_closes = 0;
};

bool TimersHostStateIsUnavailable(const TimersHostState* st) {
  return st == nullptr || st->cleanup_started;
}

constexpr int kImmediateCount = 0;
constexpr int kImmediateRefCount = 1;
constexpr int kImmediateHasOutstanding = 2;

bool TimersDebugEnabled() {
  static int enabled = -1;
  if (enabled < 0) {
    const char* env = std::getenv("EDGE_DEBUG_TIMERS");
    enabled = (env != nullptr && env[0] != '\0' && env[0] != '0') ? 1 : 0;
  }
  return enabled == 1;
}

void DebugLog(const char* fmt, ...) {
  if (!TimersDebugEnabled()) return;
  std::fprintf(stderr, "[edge-timers] ");
  va_list args;
  va_start(args, fmt);
  std::vfprintf(stderr, fmt, args);
  va_end(args);
  std::fprintf(stderr, "\n");
}

uv_loop_t* GetLoop(TimersHostState* st) {
  if (st == nullptr || st->env == nullptr) return nullptr;
  return EdgeGetEnvLoop(st->env);
}

void StopLoopOnJsError(TimersHostState* st) {
  uv_loop_t* loop = GetLoop(st);
  if (loop != nullptr) uv_stop(loop);
}

void DeleteRefIfAny(napi_env env, napi_ref* ref_slot) {
  if (env == nullptr || ref_slot == nullptr || *ref_slot == nullptr) return;
  napi_delete_reference(env, *ref_slot);
  *ref_slot = nullptr;
}

TimersHostState* GetState(napi_env env) {
  return EdgeEnvironmentGetSlotData<TimersHostState>(
      env, kEdgeEnvironmentSlotTimersHostState);
}

double GetNowMs(TimersHostState* st) {
  uv_loop_t* loop = GetLoop(st);
  if (loop == nullptr || st == nullptr) return 0;
  uv_update_time(loop);
  const double now = static_cast<double>(uv_now(loop));
  if (st->timer_base_ms < 0) {
    st->timer_base_ms = now;
  }
  const double rel = now - st->timer_base_ms;
  return rel >= 0 ? rel : 0;
}

void MaybeDestroyState(TimersHostState* st) {
  (void)st;
}

void OnHandleClosed(uv_handle_t* handle) {
  auto* st = static_cast<TimersHostState*>(handle->data);
  if (st == nullptr) return;

  if (handle == reinterpret_cast<uv_handle_t*>(&st->timer_handle)) {
    st->timer_initialized = false;
  } else if (handle == reinterpret_cast<uv_handle_t*>(&st->check_handle)) {
    st->check_initialized = false;
    st->check_running = false;
  } else if (handle == reinterpret_cast<uv_handle_t*>(&st->idle_handle)) {
    st->idle_initialized = false;
    st->idle_running = false;
  }

  if (st->pending_handle_closes > 0) {
    --st->pending_handle_closes;
  }
  MaybeDestroyState(st);
}

void CloseHandleIfInitialized(TimersHostState* st, uv_handle_t* handle, bool* initialized_flag) {
  if (st == nullptr || handle == nullptr || initialized_flag == nullptr || !*initialized_flag) return;
  *initialized_flag = false;
  if (uv_is_closing(handle) != 0) return;
  ++st->pending_handle_closes;
  uv_close(handle, OnHandleClosed);
}

void OnTimersEnvCleanup(void* arg) {
  napi_env env = static_cast<napi_env>(arg);
  TimersHostState* st = GetState(env);
  if (st == nullptr) return;

  st->cleanup_started = true;
  DeleteRefIfAny(st->env, &st->timers_callback_ref);
  DeleteRefIfAny(st->env, &st->immediate_callback_ref);
  DeleteRefIfAny(st->env, &st->immediate_info_ref);
  DeleteRefIfAny(st->env, &st->timeout_info_ref);
  if (auto* environment = EdgeEnvironmentGet(st->env); environment != nullptr) {
    DeleteRefIfAny(st->env, &environment->immediate_info()->ref);
    DeleteRefIfAny(st->env, &environment->timeout_info()->ref);
    environment->immediate_info()->fields = nullptr;
    environment->timeout_info()->fields = nullptr;
  }

  if (st->timer_initialized) {
    uv_timer_stop(&st->timer_handle);
  }
  if (st->check_initialized) {
    uv_check_stop(&st->check_handle);
    st->check_running = false;
  }
  if (st->idle_initialized) {
    uv_idle_stop(&st->idle_handle);
    st->idle_running = false;
  }

  CloseHandleIfInitialized(st, reinterpret_cast<uv_handle_t*>(&st->timer_handle), &st->timer_initialized);
  CloseHandleIfInitialized(st, reinterpret_cast<uv_handle_t*>(&st->check_handle), &st->check_initialized);
  CloseHandleIfInitialized(st, reinterpret_cast<uv_handle_t*>(&st->idle_handle), &st->idle_initialized);

  MaybeDestroyState(st);
}

TimersHostState* GetOrCreateState(napi_env env) {
  if (env == nullptr) return nullptr;
  (void)EdgeEnsureEnvLoop(env, nullptr);
  return &EdgeEnvironmentGetOrCreateSlotData<TimersHostState>(
      env, kEdgeEnvironmentSlotTimersHostState);
}

TimersHostState* GetStateForJsBindingCall(napi_env env) {
  return env != nullptr ? GetState(env) : nullptr;
}

void SetFunctionRef(napi_env env, napi_value fn, napi_ref* ref_slot) {
  if (env == nullptr || ref_slot == nullptr) return;
  DeleteRefIfAny(env, ref_slot);
  if (fn == nullptr) return;
  napi_valuetype value_type = napi_undefined;
  if (napi_typeof(env, fn, &value_type) != napi_ok || value_type != napi_function) return;
  napi_create_reference(env, fn, 1, ref_slot);
}

bool CallImmediateCallback(TimersHostState* st);
void ApplyImmediateRefState(TimersHostState* st, bool ref);

bool GetInt32ArrayDataFromRef(napi_env env,
                              napi_ref ref,
                              size_t min_length,
                              int32_t** data_out,
                              size_t* length_out = nullptr) {
  if (data_out == nullptr) return false;
  *data_out = nullptr;
  if (length_out != nullptr) *length_out = 0;
  if (env == nullptr || ref == nullptr) return false;

  napi_value value = nullptr;
  if (napi_get_reference_value(env, ref, &value) != napi_ok || value == nullptr) return false;

  bool is_typedarray = false;
  if (napi_is_typedarray(env, value, &is_typedarray) != napi_ok || !is_typedarray) return false;

  napi_typedarray_type type = napi_uint8_array;
  size_t length = 0;
  void* data = nullptr;
  napi_value arraybuffer = nullptr;
  size_t offset = 0;
  if (napi_get_typedarray_info(env, value, &type, &length, &data, &arraybuffer, &offset) != napi_ok ||
      type != napi_int32_array || data == nullptr || length < min_length) {
    return false;
  }

  *data_out = static_cast<int32_t*>(data);
  if (length_out != nullptr) *length_out = length;
  return true;
}

int32_t ActiveTimeoutCount(const TimersHostState* st) {
  int32_t* timeout_info = nullptr;
  if (st == nullptr || !GetInt32ArrayDataFromRef(st->env, st->timeout_info_ref, 1, &timeout_info)) return 0;
  const int32_t count = timeout_info[0];
  return count > 0 ? count : 0;
}

uint32_t ImmediateCount(const TimersHostState* st) {
  int32_t* immediate_info = nullptr;
  if (st == nullptr || !GetInt32ArrayDataFromRef(st->env, st->immediate_info_ref, 3, &immediate_info)) return 0;
  const int32_t count = immediate_info[kImmediateCount];
  return count > 0 ? static_cast<uint32_t>(count) : 0;
}

uint32_t ImmediateRefCount(const TimersHostState* st) {
  int32_t* immediate_info = nullptr;
  if (st == nullptr || !GetInt32ArrayDataFromRef(st->env, st->immediate_info_ref, 3, &immediate_info)) return 0;
  const int32_t count = immediate_info[kImmediateRefCount];
  return count > 0 ? static_cast<uint32_t>(count) : 0;
}

bool ImmediateHasOutstanding(const TimersHostState* st) {
  int32_t* immediate_info = nullptr;
  if (st == nullptr || !GetInt32ArrayDataFromRef(st->env, st->immediate_info_ref, 3, &immediate_info)) {
    return false;
  }
  return immediate_info[kImmediateHasOutstanding] != 0;
}

bool HasNativeImmediateTasks(const TimersHostState* st) {
  return st != nullptr && st->env != nullptr && EdgeRuntimePlatformHasImmediateTasks(st->env);
}

bool HasRefedNativeImmediateTasks(const TimersHostState* st) {
  return st != nullptr && st->env != nullptr && EdgeRuntimePlatformHasRefedImmediateTasks(st->env);
}

void EnsureTimerHandle(TimersHostState* st) {
  if (st == nullptr || st->timer_initialized) return;
  uv_loop_t* loop = GetLoop(st);
  if (loop == nullptr) return;
  if (uv_timer_init(loop, &st->timer_handle) == 0) {
    st->timer_handle.data = st;
    uv_unref(reinterpret_cast<uv_handle_t*>(&st->timer_handle));
    st->timer_initialized = true;
    DebugLog("timer handle initialized");
  }
}

void EnsureCheckHandle(TimersHostState* st) {
  if (st == nullptr || st->check_initialized) return;
  uv_loop_t* loop = GetLoop(st);
  if (loop == nullptr) return;
  if (uv_check_init(loop, &st->check_handle) != 0) return;
  st->check_handle.data = st;
  uv_unref(reinterpret_cast<uv_handle_t*>(&st->check_handle));
  if (uv_check_start(&st->check_handle,
                     [](uv_check_t* handle) {
                       auto* state = static_cast<TimersHostState*>(handle->data);
                       if (state == nullptr || state->cleanup_started) {
                         return;
                       }
                       if (ImmediateCount(state) == 0 && !HasNativeImmediateTasks(state)) {
                         if (ImmediateRefCount(state) == 0 && !HasRefedNativeImmediateTasks(state)) {
                           ApplyImmediateRefState(state, false);
                         }
                         return;
                       }
                       (void)EdgeRuntimePlatformDrainImmediateTasks(state->env);
                       if (state->env == nullptr) {
                         return;
                       }
                       bool pending = false;
                       if (napi_is_exception_pending(state->env, &pending) == napi_ok && pending) {
                         StopLoopOnJsError(state);
                         return;
                       }
                       if (ImmediateCount(state) != 0) {
                         if (!CallImmediateCallback(state)) {
                           return;
                         }
                       }

                       if (ImmediateRefCount(state) == 0 && !HasRefedNativeImmediateTasks(state)) {
                         ApplyImmediateRefState(state, false);
                       }
                     }) != 0) {
    return;
  }
  st->check_initialized = true;
  st->check_running = true;
  DebugLog("check handle initialized and started (unref)");
}

void EnsureIdleHandle(TimersHostState* st) {
  if (st == nullptr || st->idle_initialized) return;
  uv_loop_t* loop = GetLoop(st);
  if (loop == nullptr) return;
  if (uv_idle_init(loop, &st->idle_handle) == 0) {
    st->idle_handle.data = st;
    st->idle_initialized = true;
    DebugLog("idle handle initialized");
  }
}

bool CallImmediateCallback(TimersHostState* st) {
  if (st == nullptr || st->cleanup_started || st->immediate_callback_ref == nullptr) return true;
  static unsigned long long immediate_calls = 0;
  immediate_calls++;
  if (TimersDebugEnabled() && (immediate_calls <= 10 || (immediate_calls % 1000) == 0)) {
    DebugLog("CallImmediateCallback(#%llu)", immediate_calls);
  }

  napi_value cb = nullptr;
  if (napi_get_reference_value(st->env, st->immediate_callback_ref, &cb) != napi_ok || cb == nullptr) return true;
  napi_value recv = nullptr;
  if (!GetProcessReceiver(st->env, &recv) || recv == nullptr) return false;
  napi_value ignored = nullptr;
  const napi_status status = EdgeMakeCallback(st->env, recv, cb, 0, nullptr, &ignored);
  if (status != napi_ok) {
    DebugLog("CallImmediateCallback JS error (status=%d), stopping loop turn", static_cast<int>(status));
    StopLoopOnJsError(st);
    return false;
  }
  return true;
}

double CallTimersCallback(TimersHostState* st, double now) {
  if (st == nullptr || st->cleanup_started || st->timers_callback_ref == nullptr) return 0;
  DebugLog("CallTimersCallback(now=%.3f)", now);

  napi_value cb = nullptr;
  if (napi_get_reference_value(st->env, st->timers_callback_ref, &cb) != napi_ok || cb == nullptr) return 0;

  napi_value now_value = nullptr;
  if (napi_create_double(st->env, now, &now_value) != napi_ok || now_value == nullptr) return 0;

  napi_value recv = nullptr;
  if (!GetProcessReceiver(st->env, &recv) || recv == nullptr) return 0;
  napi_value result = nullptr;
  napi_status call_status = napi_ok;
  do {
    result = nullptr;
    call_status = EdgeMakeCallbackWithFlags(
        st->env,
        recv,
        cb,
        1,
        &now_value,
        &result,
        kEdgeMakeCallbackSkipTaskQueues);
    if (call_status == napi_ok && result != nullptr) break;

    bool pending = false;
    if (napi_is_exception_pending(st->env, &pending) == napi_ok && pending) {
      StopLoopOnJsError(st);
      return 0;
    }
  } while (result == nullptr && CanCallTimersCallback(st));

  if (result == nullptr) {
    if (call_status != napi_ok) {
      DebugLog("CallTimersCallback JS error (status=%d), stopping loop turn", static_cast<int>(call_status));
      StopLoopOnJsError(st);
    }
    return 0;
  }

  double next = 0;
  if (napi_get_value_double(st->env, result, &next) != napi_ok || !std::isfinite(next)) return 0;
  DebugLog("CallTimersCallback => next=%.3f", next);
  return next;
}

bool CanCallTimersCallback(TimersHostState* st) {
  if (st == nullptr || st->cleanup_started || st->env == nullptr) return false;
  if (!EdgeWorkerEnvOwnsProcessState(st->env) && EdgeWorkerEnvStopRequested(st->env)) return false;
  return true;
}

bool GetProcessReceiver(napi_env env, napi_value* recv_out) {
  if (recv_out == nullptr) return false;
  *recv_out = nullptr;
  if (env == nullptr) return false;

  napi_value global = nullptr;
  if (napi_get_global(env, &global) != napi_ok || global == nullptr) return false;

  napi_value process = nullptr;
  if (napi_get_named_property(env, global, "process", &process) == napi_ok && process != nullptr) {
    *recv_out = process;
  } else {
    *recv_out = global;
  }
  return true;
}

bool RunTimersCallbackCheckpoint(TimersHostState* st) {
  if (st == nullptr || st->cleanup_started || st->env == nullptr) return false;
  const napi_status status = EdgeRunCallbackScopeCheckpoint(st->env);
  if (status == napi_ok) return true;

  bool handled = false;
  (void)EdgeHandlePendingExceptionNow(st->env, &handled);

  bool pending = false;
  if (napi_is_exception_pending(st->env, &pending) == napi_ok && pending) {
    StopLoopOnJsError(st);
    return false;
  }

  if (status != napi_pending_exception) {
    StopLoopOnJsError(st);
    return false;
  }
  return handled;
}

void ApplyTimerRefState(TimersHostState* st, bool ref) {
  if (st == nullptr || !st->timer_initialized) return;
  if (ref) {
    uv_ref(reinterpret_cast<uv_handle_t*>(&st->timer_handle));
  } else {
    uv_unref(reinterpret_cast<uv_handle_t*>(&st->timer_handle));
  }
  DebugLog("toggleTimerRef(%s)", ref ? "true" : "false");
}

void ApplyImmediateRefState(TimersHostState* st, bool ref) {
  if (st == nullptr) return;
  EnsureCheckHandle(st);
  EnsureIdleHandle(st);
  if (!st->idle_initialized) return;
  const bool should_ref = ref || ImmediateRefCount(st) != 0 || HasRefedNativeImmediateTasks(st);
  if (should_ref) {
    if (!st->idle_running && uv_idle_start(&st->idle_handle, [](uv_idle_t* /*handle*/) {}) == 0) {
      st->idle_running = true;
    }
  } else {
    if (st->idle_running) {
      uv_idle_stop(&st->idle_handle);
      st->idle_running = false;
    }
  }
  DebugLog("toggleImmediateRef(%s)", ref ? "true" : "false");
}

void ScheduleFromNextExpiry(TimersHostState* st, double next_expiry, double now);

void RunTimersCallback(uv_timer_t* handle) {
  auto* state = static_cast<TimersHostState*>(handle->data);
  if (state == nullptr || state->cleanup_started) return;

  state->running_timers_callback = true;

  double now_ms = GetNowMs(state);
  double next = CallTimersCallback(state, now_ms);
  state->running_timers_callback = false;
  ScheduleFromNextExpiry(state, next, now_ms);
  (void)RunTimersCallbackCheckpoint(state);
}

void ScheduleFromNextExpiry(TimersHostState* st, double next_expiry, double now) {
  if (st == nullptr || !st->timer_initialized) return;
  if (next_expiry == 0 || !std::isfinite(next_expiry)) {
    uv_timer_stop(&st->timer_handle);
    ApplyTimerRefState(st, false);
    DebugLog("scheduleFromNextExpiry(next=%.3f, now=%.3f) => stop", next_expiry, now);
    return;
  }

  const bool ref = next_expiry > 0;
  const double abs_expiry = std::abs(next_expiry);
  const double delta = abs_expiry - now;
  const uint64_t timeout = static_cast<uint64_t>(delta > 1 ? delta : 1);
  DebugLog("scheduleFromNextExpiry(next=%.3f, now=%.3f, delay=%llu, ref=%s)",
           next_expiry,
           now,
           static_cast<unsigned long long>(timeout),
           ref ? "true" : "false");
  uv_timer_start(&st->timer_handle, RunTimersCallback, timeout, 0);
  ApplyTimerRefState(st, ref);
}

napi_value SetupTimers(napi_env env, napi_callback_info info) {
  TimersHostState* st = GetStateForJsBindingCall(env);
  if (st == nullptr || TimersHostStateIsUnavailable(st)) {
    napi_value undefined = nullptr;
    napi_get_undefined(env, &undefined);
    return undefined;
  }

  size_t argc = 2;
  napi_value argv[2] = {nullptr, nullptr};
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  if (argc >= 1) SetFunctionRef(env, argv[0], &st->immediate_callback_ref);
  if (argc >= 2) SetFunctionRef(env, argv[1], &st->timers_callback_ref);
  DebugLog("setupTimers(immediate=%s, timers=%s)",
           st->immediate_callback_ref ? "set" : "unset",
           st->timers_callback_ref ? "set" : "unset");

  napi_value undefined = nullptr;
  napi_get_undefined(env, &undefined);
  return undefined;
}

napi_value ScheduleTimer(napi_env env, napi_callback_info info) {
  TimersHostState* st = GetStateForJsBindingCall(env);
  if (st == nullptr || TimersHostStateIsUnavailable(st)) {
    napi_value undefined = nullptr;
    napi_get_undefined(env, &undefined);
    return undefined;
  }

  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);

  int64_t duration = 1;
  if (argc >= 1) {
    napi_get_value_int64(env, argv[0], &duration);
  }
  if (duration < 1) duration = 1;
  DebugLog("scheduleTimer(duration=%lld)", static_cast<long long>(duration));

  EnsureTimerHandle(st);
  if (st->timer_initialized) {
    uv_timer_start(&st->timer_handle, RunTimersCallback, static_cast<uint64_t>(duration), 0);
    ApplyTimerRefState(st, ActiveTimeoutCount(st) > 0);
  }

  napi_value undefined = nullptr;
  napi_get_undefined(env, &undefined);
  return undefined;
}

napi_value ToggleTimerRef(napi_env env, napi_callback_info info) {
  TimersHostState* st = GetStateForJsBindingCall(env);
  if (st == nullptr || TimersHostStateIsUnavailable(st)) {
    napi_value undefined = nullptr;
    napi_get_undefined(env, &undefined);
    return undefined;
  }

  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  bool ref = true;
  if (argc >= 1) napi_get_value_bool(env, argv[0], &ref);
  ApplyTimerRefState(st, ref);
  napi_value undefined = nullptr;
  napi_get_undefined(env, &undefined);
  return undefined;
}

napi_value ToggleImmediateRef(napi_env env, napi_callback_info info) {
  TimersHostState* st = GetStateForJsBindingCall(env);
  if (st == nullptr || TimersHostStateIsUnavailable(st)) {
    napi_value undefined = nullptr;
    napi_get_undefined(env, &undefined);
    return undefined;
  }

  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  bool ref = true;
  if (argc >= 1) napi_get_value_bool(env, argv[0], &ref);
  ApplyImmediateRefState(st, ref);
  napi_value undefined = nullptr;
  napi_get_undefined(env, &undefined);
  return undefined;
}

napi_value GetLibuvNow(napi_env env, napi_callback_info /*info*/) {
  TimersHostState* st = GetStateForJsBindingCall(env);
  napi_value out = nullptr;
  const double now = (st == nullptr || TimersHostStateIsUnavailable(st)) ? 0 : GetNowMs(st);
  napi_create_double(env, now, &out);
  return out;
}

void SetMethod(napi_env env, napi_value obj, const char* name, napi_callback cb) {
  napi_value fn = nullptr;
  if (napi_create_function(env, name, NAPI_AUTO_LENGTH, cb, nullptr, &fn) != napi_ok || fn == nullptr) {
    return;
  }
  napi_set_named_property(env, obj, name, fn);
}

void AttachInfoArrays(napi_env env, napi_value binding, TimersHostState* st) {
  if (st == nullptr) return;

  napi_value immediate_ab = nullptr;
  void* immediate_data = nullptr;
  if (napi_create_arraybuffer(env, 3 * sizeof(int32_t), &immediate_data, &immediate_ab) == napi_ok &&
      immediate_ab != nullptr && immediate_data != nullptr) {
    auto* ptr = static_cast<int32_t*>(immediate_data);
    ptr[0] = 0;
    ptr[1] = 0;
    ptr[2] = 0;
    napi_value immediate_info = nullptr;
    if (napi_create_typedarray(env, napi_int32_array, 3, immediate_ab, 0, &immediate_info) == napi_ok &&
        immediate_info != nullptr) {
      napi_set_named_property(env, binding, "immediateInfo", immediate_info);
      DeleteRefIfAny(env, &st->immediate_info_ref);
      napi_create_reference(env, immediate_info, 1, &st->immediate_info_ref);
      if (auto* environment = EdgeEnvironmentGet(env); environment != nullptr) {
        environment->immediate_info()->fields = ptr;
        DeleteRefIfAny(env, &environment->immediate_info()->ref);
        napi_create_reference(env, immediate_info, 1, &environment->immediate_info()->ref);
      }
    }
  }

  napi_value timeout_ab = nullptr;
  void* timeout_data = nullptr;
  if (napi_create_arraybuffer(env, sizeof(int32_t), &timeout_data, &timeout_ab) == napi_ok &&
      timeout_ab != nullptr && timeout_data != nullptr) {
    auto* ptr = static_cast<int32_t*>(timeout_data);
    ptr[0] = 0;
    napi_value timeout_info = nullptr;
    if (napi_create_typedarray(env, napi_int32_array, 1, timeout_ab, 0, &timeout_info) == napi_ok &&
        timeout_info != nullptr) {
      napi_set_named_property(env, binding, "timeoutInfo", timeout_info);
      DeleteRefIfAny(env, &st->timeout_info_ref);
      napi_create_reference(env, timeout_info, 1, &st->timeout_info_ref);
      if (auto* environment = EdgeEnvironmentGet(env); environment != nullptr) {
        environment->timeout_info()->fields = ptr;
        DeleteRefIfAny(env, &environment->timeout_info()->ref);
        napi_create_reference(env, timeout_info, 1, &environment->timeout_info()->ref);
      }
    }
  }
}

}  // namespace

napi_value EdgeInstallTimersHostBinding(napi_env env) {
  if (env == nullptr) return nullptr;

  if (EdgeInitializeTimersHost(env) != napi_ok) return nullptr;
  TimersHostState* st = GetState(env);
  if (st == nullptr) return nullptr;
  DebugLog("install timers host binding");

  napi_value binding = nullptr;
  if (napi_create_object(env, &binding) != napi_ok || binding == nullptr) {
    return nullptr;
  }
  SetMethod(env, binding, "setupTimers", SetupTimers);
  SetMethod(env, binding, "scheduleTimer", ScheduleTimer);
  SetMethod(env, binding, "toggleTimerRef", ToggleTimerRef);
  SetMethod(env, binding, "toggleImmediateRef", ToggleImmediateRef);
  SetMethod(env, binding, "getLibuvNow", GetLibuvNow);
  AttachInfoArrays(env, binding, st);

  return binding;
}

napi_status EdgeInitializeTimersHost(napi_env env) {
  if (env == nullptr) return napi_invalid_arg;
  TimersHostState* st = GetOrCreateState(env);
  if (st == nullptr) return napi_generic_failure;
  EnsureTimerHandle(st);
  EnsureCheckHandle(st);
  EnsureIdleHandle(st);
  return napi_ok;
}

int32_t EdgeGetActiveTimeoutCount(napi_env env) {
  const TimersHostState* st = GetState(env);
  int32_t* timeout_info = nullptr;
  if (st == nullptr || !GetInt32ArrayDataFromRef(env, st->timeout_info_ref, 1, &timeout_info)) return 0;
  const int32_t count = timeout_info[0];
  return count > 0 ? count : 0;
}

uint32_t EdgeGetActiveImmediateRefCount(napi_env env) {
  const TimersHostState* st = GetState(env);
  int32_t* immediate_info = nullptr;
  if (st == nullptr || !GetInt32ArrayDataFromRef(env, st->immediate_info_ref, 3, &immediate_info)) return 0;
  const int32_t count = immediate_info[kImmediateRefCount];
  return count > 0 ? static_cast<uint32_t>(count) : 0;
}

void EdgeEnsureTimersImmediatePump(napi_env env) {
  TimersHostState* st = GetState(env);
  if (st == nullptr) return;
  if (TimersHostStateIsUnavailable(st)) return;
  EnsureCheckHandle(st);
}

void EdgeToggleImmediateRefFromNative(napi_env env, bool ref) {
  TimersHostState* st = GetState(env);
  if (st == nullptr) return;
  if (TimersHostStateIsUnavailable(st)) return;
  ApplyImmediateRefState(st, ref);
}

void EdgeRunTimersHostEnvCleanup(napi_env env) {
  if (env == nullptr) return;
  OnTimersEnvCleanup(env);
  uv_loop_t* loop = EdgeGetExistingEnvLoop(env);
  TimersHostState* st = GetState(env);
  for (size_t guard = 0; loop != nullptr && st != nullptr && st->pending_handle_closes != 0 && guard < 1024;
       ++guard) {
    (void)uv_run(loop, UV_RUN_NOWAIT);
    st = GetState(env);
  }
}
