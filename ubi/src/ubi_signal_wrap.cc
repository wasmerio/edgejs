#include "ubi_signal_wrap.h"

#include <cstdint>
#include <map>
#include <mutex>

#include <uv.h>

// #include "ubi_async_wrap.h"
#include "ubi_runtime.h"

namespace {

struct SignalWrap {
  napi_env env = nullptr;
  napi_ref wrapper_ref = nullptr;
  napi_ref close_cb_ref = nullptr;
  uv_signal_t handle{};
  bool initialized = false;
  bool closed = false;
  bool finalized = false;
  bool delete_on_close = false;
  bool active = false;
  bool destroy_queued = false;
  // int64_t async_id = 0;
};

std::mutex g_handled_signals_mutex;
std::map<int, int64_t> g_handled_signals;

napi_value GetRefValue(napi_env env, napi_ref ref) {
  if (ref == nullptr) return nullptr;
  napi_value out = nullptr;
  if (napi_get_reference_value(env, ref, &out) != napi_ok) return nullptr;
  return out;
}

napi_value MakeInt32(napi_env env, int32_t value) {
  napi_value out = nullptr;
  napi_create_int32(env, value, &out);
  return out;
}

napi_value MakeInt64(napi_env env, int64_t value) {
  napi_value out = nullptr;
  napi_create_int64(env, value, &out);
  return out;
}

void ThrowIllegalInvocation(napi_env env) {
  napi_throw_type_error(env, nullptr, "Illegal invocation");
}

bool UnwrapSignalWrap(napi_env env, napi_value self, SignalWrap** out) {
  if (out == nullptr) return false;
  *out = nullptr;
  if (self == nullptr) {
    ThrowIllegalInvocation(env);
    return false;
  }
  SignalWrap* wrap = nullptr;
  if (napi_unwrap(env, self, reinterpret_cast<void**>(&wrap)) != napi_ok || wrap == nullptr) {
    ThrowIllegalInvocation(env);
    return false;
  }
  *out = wrap;
  return true;
}

void IncreaseSignalHandlerCount(int signum) {
  std::lock_guard<std::mutex> lock(g_handled_signals_mutex);
  g_handled_signals[signum]++;
}

void DecreaseSignalHandlerCount(int signum) {
  std::lock_guard<std::mutex> lock(g_handled_signals_mutex);
  auto it = g_handled_signals.find(signum);
  if (it == g_handled_signals.end()) return;
  if (--it->second <= 0) g_handled_signals.erase(it);
}

void QueueDestroy(SignalWrap* wrap) {
  // if (wrap == nullptr || wrap->destroy_queued || wrap->async_id <= 0) return;
  wrap->destroy_queued = true;
  // UbiAsyncWrapQueueDestroy(wrap->env, static_cast<double>(wrap->async_id));
}

void OnSignal(uv_signal_t* handle, int signum) {
  auto* wrap = static_cast<SignalWrap*>(handle->data);
  if (wrap == nullptr) return;
  napi_value self = GetRefValue(wrap->env, wrap->wrapper_ref);
  if (self == nullptr) return;
  napi_value onsignal = nullptr;
  if (napi_get_named_property(wrap->env, self, "onsignal", &onsignal) != napi_ok || onsignal == nullptr) return;
  napi_valuetype type = napi_undefined;
  if (napi_typeof(wrap->env, onsignal, &type) != napi_ok || type != napi_function) return;
  napi_value arg = MakeInt32(wrap->env, signum);
  napi_value argv[1] = {arg};
  napi_value ignored = nullptr;
  UbiMakeCallback(wrap->env, self, onsignal, 1, argv, &ignored);
}

void OnClosed(uv_handle_t* handle) {
  auto* wrap = static_cast<SignalWrap*>(handle->data);
  if (wrap == nullptr) return;
  wrap->closed = true;
  QueueDestroy(wrap);

  if (!wrap->finalized && wrap->close_cb_ref != nullptr) {
    napi_value self = GetRefValue(wrap->env, wrap->wrapper_ref);
    napi_value cb = GetRefValue(wrap->env, wrap->close_cb_ref);
    if (cb != nullptr) {
      napi_value ignored = nullptr;
      UbiMakeCallback(wrap->env, self, cb, 0, nullptr, &ignored);
    }
    napi_delete_reference(wrap->env, wrap->close_cb_ref);
    wrap->close_cb_ref = nullptr;
  }

  if (wrap->delete_on_close || wrap->finalized) {
    delete wrap;
  }
}

void SignalFinalize(napi_env env, void* data, void* /*hint*/) {
  auto* wrap = static_cast<SignalWrap*>(data);
  if (wrap == nullptr) return;
  wrap->finalized = true;
  if (wrap->wrapper_ref != nullptr) {
    napi_delete_reference(env, wrap->wrapper_ref);
    wrap->wrapper_ref = nullptr;
  }
  if (wrap->close_cb_ref != nullptr) {
    napi_delete_reference(env, wrap->close_cb_ref);
    wrap->close_cb_ref = nullptr;
  }
  if (!wrap->initialized || wrap->closed) {
    QueueDestroy(wrap);
    delete wrap;
    return;
  }
  if (wrap->active) {
    DecreaseSignalHandlerCount(wrap->handle.signum);
    wrap->active = false;
  }
  wrap->delete_on_close = true;
  uv_handle_t* handle = reinterpret_cast<uv_handle_t*>(&wrap->handle);
  if (!uv_is_closing(handle)) {
    uv_close(handle, OnClosed);
  }
  return;
}

napi_value SignalCtor(napi_env env, napi_callback_info info) {
  napi_value self = nullptr;
  size_t argc = 0;
  napi_get_cb_info(env, info, &argc, nullptr, &self, nullptr);

  auto* wrap = new SignalWrap();
  wrap->env = env;
  // wrap->async_id = UbiAsyncWrapNewAsyncId(env);

  int rc = uv_signal_init(uv_default_loop(), &wrap->handle);
  if (rc == 0) {
    wrap->initialized = true;
    wrap->handle.data = wrap;
  }
  napi_wrap(env, self, wrap, SignalFinalize, nullptr, &wrap->wrapper_ref);

  // UbiAsyncWrapEmitInit(env,
  //                        static_cast<double>(wrap->async_id),
  //                        "SIGNALWRAP",
  //                        -1,
  //                        self,
  //                        false);

  return self;
}

napi_value SignalStart(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  napi_value self = nullptr;
  napi_get_cb_info(env, info, &argc, argv, &self, nullptr);

  SignalWrap* wrap = nullptr;
  if (!UnwrapSignalWrap(env, self, &wrap)) return nullptr;
  if (!wrap->initialized) return MakeInt32(env, UV_EINVAL);
  if (argc < 1 || argv[0] == nullptr) return MakeInt32(env, UV_EINVAL);

  int32_t signum = 0;
  if (napi_get_value_int32(env, argv[0], &signum) != napi_ok) return MakeInt32(env, UV_EINVAL);

  const int rc = uv_signal_start(&wrap->handle, OnSignal, signum);
  if (rc == 0 && !wrap->active) {
    wrap->active = true;
    IncreaseSignalHandlerCount(signum);
  }
  return MakeInt32(env, rc);
}

napi_value SignalStop(napi_env env, napi_callback_info info) {
  napi_value self = nullptr;
  size_t argc = 0;
  napi_get_cb_info(env, info, &argc, nullptr, &self, nullptr);

  SignalWrap* wrap = nullptr;
  if (!UnwrapSignalWrap(env, self, &wrap)) return nullptr;
  if (!wrap->initialized) return MakeInt32(env, UV_EINVAL);

  if (wrap->active) {
    wrap->active = false;
    DecreaseSignalHandlerCount(wrap->handle.signum);
  }
  const int rc = uv_signal_stop(&wrap->handle);
  return MakeInt32(env, rc);
}

napi_value SignalClose(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  napi_value self = nullptr;
  napi_get_cb_info(env, info, &argc, argv, &self, nullptr);

  SignalWrap* wrap = nullptr;
  if (!UnwrapSignalWrap(env, self, &wrap)) return nullptr;

  if (argc >= 1 && argv[0] != nullptr) {
    napi_valuetype type = napi_undefined;
    if (napi_typeof(env, argv[0], &type) == napi_ok && type == napi_function) {
      if (wrap->close_cb_ref != nullptr) napi_delete_reference(env, wrap->close_cb_ref);
      napi_create_reference(env, argv[0], 1, &wrap->close_cb_ref);
    }
  }

  if (wrap->active) {
    DecreaseSignalHandlerCount(wrap->handle.signum);
    wrap->active = false;
  }

  if (!wrap->closed && wrap->initialized) {
    uv_handle_t* handle = reinterpret_cast<uv_handle_t*>(&wrap->handle);
    if (!uv_is_closing(handle)) {
      uv_close(handle, OnClosed);
    }
  } else {
    QueueDestroy(wrap);
  }

  napi_value out = nullptr;
  napi_get_undefined(env, &out);
  return out;
}

napi_value SignalRef(napi_env env, napi_callback_info info) {
  napi_value self = nullptr;
  size_t argc = 0;
  napi_get_cb_info(env, info, &argc, nullptr, &self, nullptr);
  SignalWrap* wrap = nullptr;
  if (!UnwrapSignalWrap(env, self, &wrap)) return nullptr;
  if (wrap->initialized) uv_ref(reinterpret_cast<uv_handle_t*>(&wrap->handle));
  napi_value out = nullptr;
  napi_get_undefined(env, &out);
  return out;
}

napi_value SignalUnref(napi_env env, napi_callback_info info) {
  napi_value self = nullptr;
  size_t argc = 0;
  napi_get_cb_info(env, info, &argc, nullptr, &self, nullptr);
  SignalWrap* wrap = nullptr;
  if (!UnwrapSignalWrap(env, self, &wrap)) return nullptr;
  if (wrap->initialized) uv_unref(reinterpret_cast<uv_handle_t*>(&wrap->handle));
  napi_value out = nullptr;
  napi_get_undefined(env, &out);
  return out;
}

napi_value SignalHasRef(napi_env env, napi_callback_info info) {
  napi_value self = nullptr;
  size_t argc = 0;
  napi_get_cb_info(env, info, &argc, nullptr, &self, nullptr);
  SignalWrap* wrap = nullptr;
  if (!UnwrapSignalWrap(env, self, &wrap)) return nullptr;
  bool has_ref = wrap->initialized &&
                 uv_has_ref(reinterpret_cast<const uv_handle_t*>(&wrap->handle)) != 0;
  napi_value out = nullptr;
  napi_get_boolean(env, has_ref, &out);
  return out;
}

// napi_value SignalGetAsyncId(napi_env env, napi_callback_info info) {
//   napi_value self = nullptr;
//   size_t argc = 0;
//   napi_get_cb_info(env, info, &argc, nullptr, &self, nullptr);
//   SignalWrap* wrap = nullptr;
//   if (!UnwrapSignalWrap(env, self, &wrap)) return nullptr;
//   return MakeInt64(env, wrap->async_id);
// }

}  // namespace

napi_value UbiInstallSignalWrapBinding(napi_env env) {
  napi_value binding = nullptr;
  if (napi_create_object(env, &binding) != napi_ok || binding == nullptr) return nullptr;

  constexpr napi_property_attributes kMethodAttrs =
      static_cast<napi_property_attributes>(napi_writable | napi_configurable);
  napi_property_descriptor signal_props[] = {
      {"start", nullptr, SignalStart, nullptr, nullptr, nullptr, kMethodAttrs, nullptr},
      {"stop", nullptr, SignalStop, nullptr, nullptr, nullptr, kMethodAttrs, nullptr},
      {"close", nullptr, SignalClose, nullptr, nullptr, nullptr, kMethodAttrs, nullptr},
      {"ref", nullptr, SignalRef, nullptr, nullptr, nullptr, kMethodAttrs, nullptr},
      {"unref", nullptr, SignalUnref, nullptr, nullptr, nullptr, kMethodAttrs, nullptr},
      {"hasRef", nullptr, SignalHasRef, nullptr, nullptr, nullptr, kMethodAttrs, nullptr},
      // {"getAsyncId", nullptr, SignalGetAsyncId, nullptr, nullptr, nullptr, kMethodAttrs, nullptr},
  };

  napi_value signal_ctor = nullptr;
  if (napi_define_class(env,
                        "Signal",
                        NAPI_AUTO_LENGTH,
                        SignalCtor,
                        nullptr,
                        sizeof(signal_props) / sizeof(signal_props[0]),
                        signal_props,
                        &signal_ctor) != napi_ok ||
      signal_ctor == nullptr) {
    return nullptr;
  }

  napi_set_named_property(env, binding, "Signal", signal_ctor);
  return binding;
}
