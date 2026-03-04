#include "ubi_timers_host.h"

#include <cstdarg>
#include <cmath>
#include <cstdio>
#include <cstdlib>

#include <uv.h>

#include "ubi_runtime.h"

namespace {

struct TimersHostState {
  napi_env env = nullptr;
  napi_ref timers_callback_ref = nullptr;
  napi_ref immediate_callback_ref = nullptr;
  uv_timer_t timer_handle{};
  uv_check_t check_handle{};
  uv_idle_t idle_handle{};
  int32_t* immediate_info_ptr = nullptr;
  int32_t* timeout_info_ptr = nullptr;
  double timer_base_ms = -1;
  bool timer_initialized = false;
  bool check_initialized = false;
  bool check_running = false;
  bool idle_initialized = false;
  bool idle_running = false;
};

TimersHostState g_timers_state;

bool TimersDebugEnabled() {
  static int enabled = -1;
  if (enabled < 0) {
    const char* env = std::getenv("UBI_DEBUG_TIMERS");
    enabled = (env != nullptr && env[0] != '\0' && env[0] != '0') ? 1 : 0;
  }
  return enabled == 1;
}

void DebugLog(const char* fmt, ...) {
  if (!TimersDebugEnabled()) return;
  std::fprintf(stderr, "[ubi-timers] ");
  va_list args;
  va_start(args, fmt);
  std::vfprintf(stderr, fmt, args);
  va_end(args);
  std::fprintf(stderr, "\n");
}

void StopLoopOnJsError() {
  uv_loop_t* loop = uv_default_loop();
  if (loop != nullptr) uv_stop(loop);
}

double GetNowMs() {
  uv_loop_t* loop = uv_default_loop();
  if (loop == nullptr) return 0;
  uv_update_time(loop);
  const double now = static_cast<double>(uv_now(loop));
  if (g_timers_state.timer_base_ms < 0) {
    g_timers_state.timer_base_ms = now;
  }
  const double rel = now - g_timers_state.timer_base_ms;
  return rel >= 0 ? rel : 0;
}

void DeleteRefIfAny(napi_env env, napi_ref* ref_slot) {
  if (env == nullptr || ref_slot == nullptr || *ref_slot == nullptr) return;
  napi_delete_reference(env, *ref_slot);
  *ref_slot = nullptr;
}

void SetFunctionRef(napi_env env, napi_value fn, napi_ref* ref_slot) {
  if (env == nullptr || ref_slot == nullptr) return;
  DeleteRefIfAny(env, ref_slot);
  if (fn == nullptr) return;
  napi_valuetype value_type = napi_undefined;
  if (napi_typeof(env, fn, &value_type) != napi_ok || value_type != napi_function) return;
  napi_create_reference(env, fn, 1, ref_slot);
}

bool HasImmediateWork() {
  if (g_timers_state.immediate_info_ptr == nullptr) return true;
  constexpr int kCount = 0;
  constexpr int kHasOutstanding = 2;
  return g_timers_state.immediate_info_ptr[kCount] > 0 ||
         g_timers_state.immediate_info_ptr[kHasOutstanding] > 0;
}

void StopImmediateLoop();
bool CallImmediateCallback();

void EnsureTimerHandle() {
  TimersHostState& st = g_timers_state;
  if (st.timer_initialized) return;
  uv_loop_t* loop = uv_default_loop();
  if (loop == nullptr) return;
  if (uv_timer_init(loop, &st.timer_handle) == 0) {
    st.timer_handle.data = &g_timers_state;
    st.timer_initialized = true;
    DebugLog("timer handle initialized");
  }
}

void EnsureCheckHandle() {
  TimersHostState& st = g_timers_state;
  if (st.check_initialized) return;
  uv_loop_t* loop = uv_default_loop();
  if (loop == nullptr) return;
  if (uv_check_init(loop, &st.check_handle) != 0) return;
  st.check_handle.data = &g_timers_state;
  uv_unref(reinterpret_cast<uv_handle_t*>(&st.check_handle));
  st.check_initialized = true;
  DebugLog("check handle initialized (unref)");
}

void EnsureIdleHandle() {
  TimersHostState& st = g_timers_state;
  if (st.idle_initialized) return;
  uv_loop_t* loop = uv_default_loop();
  if (loop == nullptr) return;
  if (uv_idle_init(loop, &st.idle_handle) == 0) {
    st.idle_handle.data = &g_timers_state;
    uv_unref(reinterpret_cast<uv_handle_t*>(&st.idle_handle));
    st.idle_initialized = true;
    DebugLog("idle handle initialized (unref)");
  }
}

bool CallImmediateCallback() {
  TimersHostState& st = g_timers_state;
  if (st.env == nullptr || st.immediate_callback_ref == nullptr) return true;
  static unsigned long long immediate_calls = 0;
  immediate_calls++;
  if (TimersDebugEnabled() && (immediate_calls <= 10 || (immediate_calls % 1000) == 0)) {
    DebugLog("CallImmediateCallback(#%llu)", immediate_calls);
  }

  napi_value cb = nullptr;
  if (napi_get_reference_value(st.env, st.immediate_callback_ref, &cb) != napi_ok || cb == nullptr) return true;
  napi_value global = nullptr;
  napi_get_global(st.env, &global);
  napi_value ignored = nullptr;
  const napi_status status = UbiMakeCallback(st.env, global, cb, 0, nullptr, &ignored);
  if (status != napi_ok) {
    DebugLog("CallImmediateCallback JS error (status=%d), stopping loop turn", static_cast<int>(status));
    StopLoopOnJsError();
    return false;
  }
  return true;
}

void StartImmediateLoop() {
  TimersHostState& st = g_timers_state;
  EnsureCheckHandle();
  EnsureIdleHandle();
  if (!st.check_initialized || st.check_running) return;
  if (uv_check_start(&st.check_handle,
                     [](uv_check_t* /*handle*/) {
                       if (!HasImmediateWork()) {
                         StopImmediateLoop();
                         return;
                       }
                       if (!CallImmediateCallback()) return;
                       if (!HasImmediateWork()) {
                         StopImmediateLoop();
                       }
                     }) == 0) {
    st.check_running = true;
    if (st.idle_initialized && !st.idle_running) {
      if (uv_idle_start(&st.idle_handle, [](uv_idle_t* /*handle*/) {}) == 0) {
        st.idle_running = true;
      }
    }
    DebugLog("immediate loop started");
  }
}

void StopImmediateLoop() {
  TimersHostState& st = g_timers_state;
  if (st.check_initialized && st.check_running) {
    uv_check_stop(&st.check_handle);
    st.check_running = false;
    DebugLog("immediate loop stopped");
  }
  if (st.idle_initialized && st.idle_running) {
    uv_idle_stop(&st.idle_handle);
    st.idle_running = false;
  }
}

double CallTimersCallback(double now) {
  TimersHostState& st = g_timers_state;
  if (st.env == nullptr || st.timers_callback_ref == nullptr) return 0;
  DebugLog("CallTimersCallback(now=%.3f)", now);

  napi_value cb = nullptr;
  if (napi_get_reference_value(st.env, st.timers_callback_ref, &cb) != napi_ok || cb == nullptr) return 0;

  napi_value now_value = nullptr;
  if (napi_create_double(st.env, now, &now_value) != napi_ok || now_value == nullptr) return 0;

  napi_value global = nullptr;
  napi_get_global(st.env, &global);
  napi_value result = nullptr;
  const napi_status call_status = UbiMakeCallback(st.env, global, cb, 1, &now_value, &result);
  if (call_status != napi_ok || result == nullptr) {
    DebugLog("CallTimersCallback JS error (status=%d), stopping loop turn", static_cast<int>(call_status));
    StopLoopOnJsError();
    return 0;
  }

  double next = 0;
  if (napi_get_value_double(st.env, result, &next) != napi_ok || !std::isfinite(next)) return 0;
  DebugLog("CallTimersCallback => next=%.3f", next);
  return next;
}

void ApplyTimerRefState(bool ref) {
  TimersHostState& st = g_timers_state;
  if (!st.timer_initialized) return;
  if (ref) {
    uv_ref(reinterpret_cast<uv_handle_t*>(&st.timer_handle));
  } else {
    uv_unref(reinterpret_cast<uv_handle_t*>(&st.timer_handle));
  }
  DebugLog("toggleTimerRef(%s)", ref ? "true" : "false");
}

void ApplyImmediateRefState(bool ref) {
  TimersHostState& st = g_timers_state;
  EnsureCheckHandle();
  if (!st.check_initialized) return;
  if (ref) {
    uv_ref(reinterpret_cast<uv_handle_t*>(&st.check_handle));
    // Node can toggle immediate ref before JS increments kCount.
    // Start the loop unconditionally for refed immediates.
    StartImmediateLoop();
  } else {
    uv_unref(reinterpret_cast<uv_handle_t*>(&st.check_handle));
    if (HasImmediateWork()) StartImmediateLoop();
    else StopImmediateLoop();
  }
  DebugLog("toggleImmediateRef(%s)", ref ? "true" : "false");
}

void ScheduleFromNextExpiry(double next_expiry, double now) {
  TimersHostState& st = g_timers_state;
  if (!st.timer_initialized) return;
  if (next_expiry == 0 || !std::isfinite(next_expiry)) return;

  const bool ref = next_expiry > 0;
  const double abs_expiry = std::abs(next_expiry);
  const double delta = abs_expiry - now;
  const uint64_t timeout = static_cast<uint64_t>(delta > 1 ? delta : 1);
  DebugLog("scheduleFromNextExpiry(next=%.3f, now=%.3f, delay=%llu, ref=%s)",
           next_expiry,
           now,
           static_cast<unsigned long long>(timeout),
           ref ? "true" : "false");
  uv_timer_start(&st.timer_handle,
                 [](uv_timer_t* /*handle*/) {
                   const double now_ms = GetNowMs();
                   const double next = CallTimersCallback(now_ms);
                   ScheduleFromNextExpiry(next, now_ms);
                 },
                 timeout,
                 0);
  ApplyTimerRefState(ref);
}

napi_value SetupTimers(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value argv[2] = {nullptr, nullptr};
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  if (argc >= 1) SetFunctionRef(env, argv[0], &g_timers_state.immediate_callback_ref);
  if (argc >= 2) SetFunctionRef(env, argv[1], &g_timers_state.timers_callback_ref);
  if (HasImmediateWork()) StartImmediateLoop();
  DebugLog("setupTimers(immediate=%s, timers=%s)",
           g_timers_state.immediate_callback_ref ? "set" : "unset",
           g_timers_state.timers_callback_ref ? "set" : "unset");

  napi_value undefined = nullptr;
  napi_get_undefined(env, &undefined);
  return undefined;
}

napi_value ScheduleTimer(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);

  int64_t duration = 1;
  if (argc >= 1) {
    napi_get_value_int64(env, argv[0], &duration);
  }
  if (duration < 1) duration = 1;
  DebugLog("scheduleTimer(duration=%lld)", static_cast<long long>(duration));

  EnsureTimerHandle();
  if (g_timers_state.timer_initialized) {
    uv_timer_start(&g_timers_state.timer_handle,
                   [](uv_timer_t* /*handle*/) {
                     const double now_ms = GetNowMs();
                     const double next = CallTimersCallback(now_ms);
                     ScheduleFromNextExpiry(next, now_ms);
                   },
                   static_cast<uint64_t>(duration),
                   0);
  }

  napi_value undefined = nullptr;
  napi_get_undefined(env, &undefined);
  return undefined;
}

napi_value ToggleTimerRef(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  bool ref = true;
  if (argc >= 1) napi_get_value_bool(env, argv[0], &ref);
  ApplyTimerRefState(ref);
  napi_value undefined = nullptr;
  napi_get_undefined(env, &undefined);
  return undefined;
}

napi_value ToggleImmediateRef(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  bool ref = true;
  if (argc >= 1) napi_get_value_bool(env, argv[0], &ref);
  ApplyImmediateRefState(ref);
  napi_value undefined = nullptr;
  napi_get_undefined(env, &undefined);
  return undefined;
}

napi_value GetLibuvNow(napi_env env, napi_callback_info /*info*/) {
  napi_value out = nullptr;
  napi_create_double(env, GetNowMs(), &out);
  return out;
}

void SetMethod(napi_env env, napi_value obj, const char* name, napi_callback cb) {
  napi_value fn = nullptr;
  if (napi_create_function(env, name, NAPI_AUTO_LENGTH, cb, nullptr, &fn) != napi_ok || fn == nullptr) {
    return;
  }
  napi_set_named_property(env, obj, name, fn);
}

void AttachInfoArrays(napi_env env, napi_value binding) {
  napi_value immediate_ab = nullptr;
  void* immediate_data = nullptr;
  if (napi_create_arraybuffer(env, 3 * sizeof(int32_t), &immediate_data, &immediate_ab) == napi_ok &&
      immediate_ab != nullptr && immediate_data != nullptr) {
    auto* ptr = static_cast<int32_t*>(immediate_data);
    ptr[0] = 0;
    ptr[1] = 0;
    ptr[2] = 0;
    g_timers_state.immediate_info_ptr = ptr;
    napi_value immediate_info = nullptr;
    if (napi_create_typedarray(env, napi_int32_array, 3, immediate_ab, 0, &immediate_info) == napi_ok &&
        immediate_info != nullptr) {
      napi_set_named_property(env, binding, "immediateInfo", immediate_info);
    }
  }

  napi_value timeout_ab = nullptr;
  void* timeout_data = nullptr;
  if (napi_create_arraybuffer(env, sizeof(int32_t), &timeout_data, &timeout_ab) == napi_ok &&
      timeout_ab != nullptr && timeout_data != nullptr) {
    auto* ptr = static_cast<int32_t*>(timeout_data);
    ptr[0] = 0;
    g_timers_state.timeout_info_ptr = ptr;
    napi_value timeout_info = nullptr;
    if (napi_create_typedarray(env, napi_int32_array, 1, timeout_ab, 0, &timeout_info) == napi_ok &&
        timeout_info != nullptr) {
      napi_set_named_property(env, binding, "timeoutInfo", timeout_info);
    }
  }
}

}  // namespace

void UbiInstallTimersHostBinding(napi_env env) {
  if (env == nullptr) return;

  g_timers_state.env = env;
  EnsureTimerHandle();
  EnsureCheckHandle();
  EnsureIdleHandle();
  DebugLog("install timers host binding");

  napi_value binding = nullptr;
  if (napi_create_object(env, &binding) != napi_ok || binding == nullptr) {
    return;
  }
  SetMethod(env, binding, "setupTimers", SetupTimers);
  SetMethod(env, binding, "scheduleTimer", ScheduleTimer);
  SetMethod(env, binding, "toggleTimerRef", ToggleTimerRef);
  SetMethod(env, binding, "toggleImmediateRef", ToggleImmediateRef);
  SetMethod(env, binding, "getLibuvNow", GetLibuvNow);
  AttachInfoArrays(env, binding);

  napi_value global = nullptr;
  if (napi_get_global(env, &global) != napi_ok || global == nullptr) {
    return;
  }
  napi_set_named_property(env, global, "__ubi_timers_binding", binding);
}
