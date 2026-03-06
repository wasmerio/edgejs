#include "internal_binding/dispatch.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>
#include <uv.h>

#include "internal_binding/helpers.h"
#include "../ubi_module_loader.h"
#include "../ubi_path.h"
#include "ubi_active_resource.h"
#include "ubi_runtime.h"

namespace internal_binding {

namespace {

constexpr size_t kFsStatsLength = 18;
constexpr size_t kFsStatFsLength = 7;

struct FsBindingState {
  napi_ref binding_ref = nullptr;
  std::unordered_map<std::string, napi_ref> raw_methods;
  napi_ref file_handle_ctor_ref = nullptr;
  napi_ref fs_req_ctor_ref = nullptr;
  napi_ref stat_watcher_ctor_ref = nullptr;
  napi_ref k_use_promises_symbol_ref = nullptr;
};

std::unordered_map<napi_env, FsBindingState> g_fs_states;
int64_t g_next_file_handle_async_id = 600000;
int64_t g_next_stat_watcher_async_id = 700000;

FsBindingState* GetState(napi_env env) {
  auto it = g_fs_states.find(env);
  if (it == g_fs_states.end()) return nullptr;
  return &it->second;
}

napi_value GetRefValue(napi_env env, napi_ref ref) {
  if (ref == nullptr) return nullptr;
  napi_value out = nullptr;
  if (napi_get_reference_value(env, ref, &out) != napi_ok || out == nullptr) return nullptr;
  return out;
}

napi_value GetBinding(napi_env env) {
  FsBindingState* st = GetState(env);
  return st == nullptr ? nullptr : GetRefValue(env, st->binding_ref);
}

void ResetRef(napi_env env, napi_ref* ref_ptr) {
  if (ref_ptr == nullptr || *ref_ptr == nullptr) return;
  napi_delete_reference(env, *ref_ptr);
  *ref_ptr = nullptr;
}

void SetNamedMethod(napi_env env, napi_value obj, const char* name, napi_callback cb) {
  napi_value fn = nullptr;
  if (napi_create_function(env, name, NAPI_AUTO_LENGTH, cb, nullptr, &fn) == napi_ok && fn != nullptr) {
    napi_set_named_property(env, obj, name, fn);
  }
}

void SetNamedInt(napi_env env, napi_value obj, const char* name, int32_t value) {
  napi_value v = nullptr;
  if (napi_create_int32(env, value, &v) == napi_ok && v != nullptr) napi_set_named_property(env, obj, name, v);
}

napi_value MakeInt32(napi_env env, int32_t value) {
  napi_value out = nullptr;
  napi_create_int32(env, value, &out);
  return out != nullptr ? out : Undefined(env);
}

void DeleteNamedProperty(napi_env env, napi_value obj, const char* name) {
  if (env == nullptr || obj == nullptr || name == nullptr) return;
  napi_value key = nullptr;
  if (napi_create_string_utf8(env, name, NAPI_AUTO_LENGTH, &key) != napi_ok || key == nullptr) return;
  bool deleted = false;
  (void)napi_delete_property(env, obj, key, &deleted);
}

bool ValueToUtf8(napi_env env, napi_value value, std::string* out) {
  if (value == nullptr || out == nullptr) return false;
  size_t len = 0;
  if (napi_get_value_string_utf8(env, value, nullptr, 0, &len) != napi_ok) return false;
  std::string text(len + 1, '\0');
  size_t copied = 0;
  if (napi_get_value_string_utf8(env, value, text.data(), text.size(), &copied) != napi_ok) return false;
  text.resize(copied);
  *out = std::move(text);
  return true;
}

size_t TypedArrayElementSize(napi_typedarray_type type) {
  switch (type) {
    case napi_int8_array:
    case napi_uint8_array:
    case napi_uint8_clamped_array:
      return 1;
    case napi_int16_array:
    case napi_uint16_array:
      return 2;
    case napi_int32_array:
    case napi_uint32_array:
    case napi_float32_array:
      return 4;
    case napi_float64_array:
    case napi_bigint64_array:
    case napi_biguint64_array:
      return 8;
    default:
      return 1;
  }
}

bool IsBufferEncoding(napi_env env, napi_value value) {
  std::string encoding;
  return ValueToUtf8(env, value, &encoding) && encoding == "buffer";
}

bool CaptureRawMethod(napi_env env, FsBindingState* st, napi_value binding, const char* name) {
  if (st == nullptr || binding == nullptr || name == nullptr) return false;
  if (st->raw_methods.find(name) != st->raw_methods.end()) return true;
  napi_value fn = nullptr;
  if (napi_get_named_property(env, binding, name, &fn) != napi_ok || fn == nullptr) return false;
  napi_valuetype t = napi_undefined;
  if (napi_typeof(env, fn, &t) != napi_ok || t != napi_function) return false;
  napi_ref ref = nullptr;
  if (napi_create_reference(env, fn, 1, &ref) != napi_ok || ref == nullptr) return false;
  st->raw_methods.emplace(name, ref);
  return true;
}

bool CallRaw(napi_env env,
             const char* method,
             size_t argc,
             napi_value* argv,
             napi_value* out,
             napi_value* error = nullptr) {
  FsBindingState* st = GetState(env);
  napi_value binding = GetBinding(env);
  if (out != nullptr) *out = nullptr;
  if (error != nullptr) *error = nullptr;
  if (st == nullptr || binding == nullptr || method == nullptr) return false;
  auto it = st->raw_methods.find(method);
  if (it == st->raw_methods.end()) return false;
  napi_value fn = GetRefValue(env, it->second);
  if (fn == nullptr) return false;
  if (napi_call_function(env, binding, fn, argc, argv, out) == napi_ok && (out == nullptr || *out != nullptr)) {
    return true;
  }
  bool pending = false;
  if (napi_is_exception_pending(env, &pending) == napi_ok && pending) {
    napi_value err = nullptr;
    napi_get_and_clear_last_exception(env, &err);
    if (error != nullptr) *error = err;
  }
  return false;
}

bool ErrorCodeEquals(napi_env env, napi_value error, const char* code) {
  if (error == nullptr || code == nullptr) return false;
  napi_value value = nullptr;
  if (napi_get_named_property(env, error, "code", &value) != napi_ok || value == nullptr) return false;
  std::string text;
  return ValueToUtf8(env, value, &text) && text == code;
}

napi_value CreateTypedStatsArray(napi_env env, size_t length, bool as_bigint, napi_value source) {
  napi_value ab = nullptr;
  void* data = nullptr;
  const size_t byte_length = (as_bigint ? sizeof(int64_t) : sizeof(double)) * length;
  if (napi_create_arraybuffer(env, byte_length, &data, &ab) != napi_ok || ab == nullptr || data == nullptr) {
    return Undefined(env);
  }
  napi_value out = nullptr;
  const napi_typedarray_type type = as_bigint ? napi_bigint64_array : napi_float64_array;
  if (napi_create_typedarray(env, type, length, ab, 0, &out) != napi_ok || out == nullptr) {
    return Undefined(env);
  }

  if (as_bigint) {
    auto* values = static_cast<int64_t*>(data);
    for (size_t i = 0; i < length; ++i) values[i] = 0;
    if (source != nullptr) {
      for (size_t i = 0; i < length; ++i) {
        napi_value item = nullptr;
        if (napi_get_element(env, source, static_cast<uint32_t>(i), &item) != napi_ok || item == nullptr) continue;
        int64_t int64_value = 0;
        bool lossless = false;
        if (napi_get_value_bigint_int64(env, item, &int64_value, &lossless) == napi_ok) {
          values[i] = int64_value;
          continue;
        }
        double number = 0;
        if (napi_get_value_double(env, item, &number) == napi_ok) values[i] = static_cast<int64_t>(number);
      }
    }
  } else {
    auto* values = static_cast<double*>(data);
    for (size_t i = 0; i < length; ++i) values[i] = 0;
    if (source != nullptr) {
      for (size_t i = 0; i < length; ++i) {
        napi_value item = nullptr;
        if (napi_get_element(env, source, static_cast<uint32_t>(i), &item) != napi_ok || item == nullptr) continue;
        double number = 0;
        if (napi_get_value_double(env, item, &number) == napi_ok) values[i] = number;
      }
    }
  }

  return out;
}

void PopulateStatsArrayFromUv(const uv_stat_t* stat, double* out) {
  if (stat == nullptr || out == nullptr) return;
  out[0] = static_cast<double>(stat->st_dev);
  out[1] = static_cast<double>(stat->st_mode);
  out[2] = static_cast<double>(stat->st_nlink);
  out[3] = static_cast<double>(stat->st_uid);
  out[4] = static_cast<double>(stat->st_gid);
  out[5] = static_cast<double>(stat->st_rdev);
  out[6] = static_cast<double>(stat->st_blksize);
  out[7] = static_cast<double>(stat->st_ino);
  out[8] = static_cast<double>(stat->st_size);
  out[9] = static_cast<double>(stat->st_blocks);
  out[10] = static_cast<double>(stat->st_atim.tv_sec);
  out[11] = static_cast<double>(stat->st_atim.tv_nsec);
  out[12] = static_cast<double>(stat->st_mtim.tv_sec);
  out[13] = static_cast<double>(stat->st_mtim.tv_nsec);
  out[14] = static_cast<double>(stat->st_ctim.tv_sec);
  out[15] = static_cast<double>(stat->st_ctim.tv_nsec);
  out[16] = static_cast<double>(stat->st_birthtim.tv_sec);
  out[17] = static_cast<double>(stat->st_birthtim.tv_nsec);
}

napi_value CreateStatWatcherArray(napi_env env, bool as_bigint, const uv_stat_t* curr, const uv_stat_t* prev) {
  const size_t total_length = kFsStatsLength * 2;
  napi_value ab = nullptr;
  void* data = nullptr;
  const size_t byte_length = (as_bigint ? sizeof(int64_t) : sizeof(double)) * total_length;
  if (napi_create_arraybuffer(env, byte_length, &data, &ab) != napi_ok || ab == nullptr || data == nullptr) {
    return Undefined(env);
  }

  napi_value out = nullptr;
  const napi_typedarray_type type = as_bigint ? napi_bigint64_array : napi_float64_array;
  if (napi_create_typedarray(env, type, total_length, ab, 0, &out) != napi_ok || out == nullptr) {
    return Undefined(env);
  }

  double curr_values[kFsStatsLength] = {};
  double prev_values[kFsStatsLength] = {};
  PopulateStatsArrayFromUv(curr, curr_values);
  PopulateStatsArrayFromUv(prev, prev_values);

  if (as_bigint) {
    auto* values = static_cast<int64_t*>(data);
    for (size_t i = 0; i < kFsStatsLength; ++i) {
      values[i] = static_cast<int64_t>(curr_values[i]);
      values[i + kFsStatsLength] = static_cast<int64_t>(prev_values[i]);
    }
  } else {
    auto* values = static_cast<double*>(data);
    for (size_t i = 0; i < kFsStatsLength; ++i) {
      values[i] = curr_values[i];
      values[i + kFsStatsLength] = prev_values[i];
    }
  }

  return out;
}

napi_value GetUsePromisesSymbol(napi_env env) {
  FsBindingState* st = GetState(env);
  if (st == nullptr) return nullptr;
  napi_value symbol = GetRefValue(env, st->k_use_promises_symbol_ref);
  if (symbol != nullptr) return symbol;
  napi_value description = nullptr;
  napi_create_string_utf8(env, "fs_use_promises_symbol", NAPI_AUTO_LENGTH, &description);
  napi_create_symbol(env, description, &symbol);
  if (symbol != nullptr) napi_create_reference(env, symbol, 1, &st->k_use_promises_symbol_ref);
  return symbol;
}

napi_value BufferFromValue(napi_env env, napi_value value, const char* encoding) {
  bool is_buffer = false;
  if (value != nullptr && napi_is_buffer(env, value, &is_buffer) == napi_ok && is_buffer) return value;

  napi_value global = GetGlobal(env);
  napi_value buffer_ctor = nullptr;
  if (global != nullptr &&
      napi_get_named_property(env, global, "Buffer", &buffer_ctor) == napi_ok &&
      buffer_ctor != nullptr &&
      !IsUndefined(env, buffer_ctor)) {
    // Use global Buffer.
  } else {
    napi_value require_fn = UbiGetRequireFunction(env);
    napi_valuetype require_type = napi_undefined;
    if ((global == nullptr && require_fn == nullptr) ||
        ((require_fn == nullptr || IsUndefined(env, require_fn)) &&
         napi_get_named_property(env, global, "require", &require_fn) != napi_ok) ||
        require_fn == nullptr ||
        napi_typeof(env, require_fn, &require_type) != napi_ok ||
        require_type != napi_function) {
      return Undefined(env);
    }
    napi_value module_name = nullptr;
    napi_create_string_utf8(env, "buffer", NAPI_AUTO_LENGTH, &module_name);
    napi_value buffer_module = nullptr;
    if (napi_call_function(env, global, require_fn, 1, &module_name, &buffer_module) != napi_ok ||
        buffer_module == nullptr ||
        napi_get_named_property(env, buffer_module, "Buffer", &buffer_ctor) != napi_ok ||
        buffer_ctor == nullptr) {
      return Undefined(env);
    }
  }
  napi_value from_fn = nullptr;
  napi_valuetype t = napi_undefined;
  if (napi_get_named_property(env, buffer_ctor, "from", &from_fn) != napi_ok ||
      from_fn == nullptr ||
      napi_typeof(env, from_fn, &t) != napi_ok ||
      t != napi_function) {
    return Undefined(env);
  }
  napi_value argv[2] = {value != nullptr ? value : Undefined(env), nullptr};
  size_t argc = 1;
  if (encoding != nullptr) {
    napi_create_string_utf8(env, encoding, NAPI_AUTO_LENGTH, &argv[1]);
    if (argv[1] != nullptr) argc = 2;
  }
  napi_value out = nullptr;
  if (napi_call_function(env, buffer_ctor, from_fn, argc, argv, &out) != napi_ok || out == nullptr) {
    return Undefined(env);
  }
  return out;
}

size_t ByteLengthOfValue(napi_env env, napi_value value) {
  if (value == nullptr) return 0;
  bool is_buffer = false;
  if (napi_is_buffer(env, value, &is_buffer) == napi_ok && is_buffer) {
    void* data = nullptr;
    size_t len = 0;
    if (napi_get_buffer_info(env, value, &data, &len) == napi_ok) return len;
  }
  bool is_typed_array = false;
  if (napi_is_typedarray(env, value, &is_typed_array) == napi_ok && is_typed_array) {
    napi_typedarray_type type = napi_uint8_array;
    size_t len = 0;
    void* data = nullptr;
    napi_value ab = nullptr;
    size_t offset = 0;
    if (napi_get_typedarray_info(env, value, &type, &len, &data, &ab, &offset) == napi_ok) {
      return len * TypedArrayElementSize(type);
    }
  }
  bool is_dataview = false;
  if (napi_is_dataview(env, value, &is_dataview) == napi_ok && is_dataview) {
    size_t len = 0;
    void* data = nullptr;
    napi_value ab = nullptr;
    size_t offset = 0;
    if (napi_get_dataview_info(env, value, &len, &data, &ab, &offset) == napi_ok) return len;
  }
  bool is_arraybuffer = false;
  if (napi_is_arraybuffer(env, value, &is_arraybuffer) == napi_ok && is_arraybuffer) {
    void* data = nullptr;
    size_t len = 0;
    if (napi_get_arraybuffer_info(env, value, &data, &len) == napi_ok) return len;
  }
  return 0;
}

napi_value ConvertNameArrayToEncoding(napi_env env, napi_value names, bool as_buffer) {
  if (!as_buffer || names == nullptr || IsUndefined(env, names)) return names;
  uint32_t len = 0;
  bool is_array = false;
  if (napi_is_array(env, names, &is_array) != napi_ok || !is_array ||
      napi_get_array_length(env, names, &len) != napi_ok) {
    return names;
  }
  napi_value out = nullptr;
  if (napi_create_array_with_length(env, len, &out) != napi_ok || out == nullptr) return names;
  for (uint32_t i = 0; i < len; ++i) {
    napi_value item = nullptr;
    if (napi_get_element(env, names, i, &item) != napi_ok || item == nullptr) continue;
    napi_value converted = BufferFromValue(env, item, "utf8");
    if (converted == nullptr || IsUndefined(env, converted)) converted = item;
    napi_set_element(env, out, i, converted);
  }
  return out;
}

napi_value MakeResolvedPromise(napi_env env, napi_value value) {
  napi_deferred deferred = nullptr;
  napi_value promise = nullptr;
  if (napi_create_promise(env, &deferred, &promise) != napi_ok || promise == nullptr) return Undefined(env);
  napi_resolve_deferred(env, deferred, value != nullptr ? value : Undefined(env));
  return promise;
}

napi_value MakeRejectedPromise(napi_env env, napi_value reason) {
  napi_deferred deferred = nullptr;
  napi_value promise = nullptr;
  if (napi_create_promise(env, &deferred, &promise) != napi_ok || promise == nullptr) return Undefined(env);
  napi_reject_deferred(env, deferred, reason != nullptr ? reason : Undefined(env));
  return promise;
}

enum class ReqKind {
  kNone,
  kPromise,
  kCallback,
};

struct DeferredReqCompletion {
  napi_env env = nullptr;
  napi_ref req_ref = nullptr;
  napi_ref oncomplete_ref = nullptr;
  napi_ref err_ref = nullptr;
  napi_ref value_ref = nullptr;
};

void TrackActiveRequest(napi_env env, napi_value req) {
  if (req == nullptr || IsUndefined(env, req)) return;
  UbiTrackActiveRequest(env, req, "FSReqCallback");
}

void UntrackActiveRequest(napi_env env, napi_value req) {
  if (req == nullptr || IsUndefined(env, req)) return;
  UbiUntrackActiveRequest(env, req);
}

void DestroyDeferredReqCompletion(DeferredReqCompletion* completion) {
  if (completion == nullptr) return;
  napi_env env = completion->env;
  if (env != nullptr) {
    ResetRef(env, &completion->req_ref);
    ResetRef(env, &completion->oncomplete_ref);
    ResetRef(env, &completion->err_ref);
    ResetRef(env, &completion->value_ref);
  }
  delete completion;
}

void InvokeDeferredReqCompletion(napi_env env, DeferredReqCompletion* completion) {
  if (completion == nullptr || env == nullptr) return;
  napi_value req = GetRefValue(env, completion->req_ref);
  napi_value oncomplete = GetRefValue(env, completion->oncomplete_ref);
  napi_value err = GetRefValue(env, completion->err_ref);
  napi_value value = GetRefValue(env, completion->value_ref);
  if (req != nullptr) UntrackActiveRequest(env, req);

  if (req == nullptr || oncomplete == nullptr) {
    DestroyDeferredReqCompletion(completion);
    return;
  }

  if (err != nullptr && !IsUndefined(env, err)) {
    napi_value argv[1] = {err};
    napi_value ignored = nullptr;
    napi_call_function(env, req, oncomplete, 1, argv, &ignored);
  } else if (value != nullptr && !IsUndefined(env, value)) {
    napi_value argv[2] = {Undefined(env), value};
    napi_value ignored = nullptr;
    napi_call_function(env, req, oncomplete, 2, argv, &ignored);
  } else {
    napi_value argv[1] = {Undefined(env)};
    napi_value ignored = nullptr;
    napi_call_function(env, req, oncomplete, 1, argv, &ignored);
  }
  DestroyDeferredReqCompletion(completion);
}

napi_value DeferredReqCompletionCallback(napi_env env, napi_callback_info info) {
  void* data = nullptr;
  size_t argc = 0;
  napi_get_cb_info(env, info, &argc, nullptr, nullptr, &data);
  auto* completion = static_cast<DeferredReqCompletion*>(data);
  InvokeDeferredReqCompletion(env, completion);
  return Undefined(env);
}

bool ScheduleDeferredReqCompletion(napi_env env,
                                   napi_value req,
                                   napi_value oncomplete,
                                   napi_value err,
                                   napi_value value) {
  if (env == nullptr || req == nullptr || oncomplete == nullptr) return false;
  auto* completion = new DeferredReqCompletion();
  completion->env = env;
  if (napi_create_reference(env, req, 1, &completion->req_ref) != napi_ok ||
      napi_create_reference(env, oncomplete, 1, &completion->oncomplete_ref) != napi_ok ||
      (err != nullptr && !IsUndefined(env, err) &&
       napi_create_reference(env, err, 1, &completion->err_ref) != napi_ok) ||
      (value != nullptr && !IsUndefined(env, value) &&
       napi_create_reference(env, value, 1, &completion->value_ref) != napi_ok)) {
    DestroyDeferredReqCompletion(completion);
    return false;
  }

  TrackActiveRequest(env, req);

  napi_value callback = nullptr;
  if (napi_create_function(env,
                           "__ubiFsDeferredReqComplete",
                           NAPI_AUTO_LENGTH,
                           DeferredReqCompletionCallback,
                           completion,
                           &callback) != napi_ok ||
      callback == nullptr) {
    UntrackActiveRequest(env, req);
    DestroyDeferredReqCompletion(completion);
    return false;
  }

  napi_value global = GetGlobal(env);
  napi_value process = nullptr;
  napi_value next_tick = nullptr;
  napi_valuetype next_tick_type = napi_undefined;
  if (global == nullptr ||
      napi_get_named_property(env, global, "process", &process) != napi_ok ||
      process == nullptr ||
      napi_get_named_property(env, process, "nextTick", &next_tick) != napi_ok ||
      next_tick == nullptr ||
      napi_typeof(env, next_tick, &next_tick_type) != napi_ok ||
      next_tick_type != napi_function) {
    UntrackActiveRequest(env, req);
    DestroyDeferredReqCompletion(completion);
    return false;
  }

  napi_value argv[1] = {callback};
  napi_value ignored = nullptr;
  if (napi_call_function(env, process, next_tick, 1, argv, &ignored) != napi_ok) {
    UntrackActiveRequest(env, req);
    DestroyDeferredReqCompletion(completion);
    return false;
  }

  return true;
}

ReqKind ParseReq(napi_env env, napi_value candidate, napi_value* oncomplete) {
  if (oncomplete != nullptr) *oncomplete = nullptr;
  if (candidate == nullptr || IsUndefined(env, candidate)) return ReqKind::kNone;

  napi_value symbol = GetUsePromisesSymbol(env);
  if (symbol != nullptr) {
    bool same = false;
    if (napi_strict_equals(env, candidate, symbol, &same) == napi_ok && same) return ReqKind::kPromise;
  }

  napi_valuetype t = napi_undefined;
  if (napi_typeof(env, candidate, &t) == napi_ok && t == napi_object) {
    napi_value fn = nullptr;
    napi_valuetype fn_t = napi_undefined;
    if (napi_get_named_property(env, candidate, "oncomplete", &fn) == napi_ok &&
        fn != nullptr &&
        napi_typeof(env, fn, &fn_t) == napi_ok &&
        fn_t == napi_function) {
      if (oncomplete != nullptr) *oncomplete = fn;
      return ReqKind::kCallback;
    }
  }
  return ReqKind::kNone;
}

void CompleteReq(napi_env env, ReqKind kind, napi_value req, napi_value oncomplete, napi_value err, napi_value value) {
  if (kind != ReqKind::kCallback || req == nullptr || oncomplete == nullptr) return;
  if (ScheduleDeferredReqCompletion(env, req, oncomplete, err, value)) return;
  if (err != nullptr && !IsUndefined(env, err)) {
    napi_value argv[1] = {err};
    napi_value ignored = nullptr;
    napi_call_function(env, req, oncomplete, 1, argv, &ignored);
    return;
  }
  if (value != nullptr && !IsUndefined(env, value)) {
    napi_value argv[2] = {Undefined(env), value};
    napi_value ignored = nullptr;
    napi_call_function(env, req, oncomplete, 2, argv, &ignored);
    return;
  }
  napi_value argv[1] = {Undefined(env)};
  napi_value ignored = nullptr;
  napi_call_function(env, req, oncomplete, 1, argv, &ignored);
}

napi_value CompleteVoidRawFsMethod(napi_env env,
                                   const char* method,
                                   ReqKind req_kind,
                                   napi_value req,
                                   napi_value oncomplete,
                                   size_t argc,
                                   napi_value* argv) {
  napi_value out = nullptr;
  napi_value err = nullptr;
  if (!CallRaw(env, method, argc, argv, &out, &err)) {
    if (req_kind == ReqKind::kPromise) return MakeRejectedPromise(env, err);
    CompleteReq(env, req_kind, req, oncomplete, err, Undefined(env));
    if (req_kind == ReqKind::kCallback) return Undefined(env);
    if (err != nullptr) {
      napi_throw(env, err);
      return nullptr;
    }
    return Undefined(env);
  }

  if (req_kind == ReqKind::kPromise) return MakeResolvedPromise(env, Undefined(env));
  if (req_kind == ReqKind::kCallback) {
    CompleteReq(env, req_kind, req, oncomplete, nullptr, Undefined(env));
    return Undefined(env);
  }
  return out != nullptr ? out : Undefined(env);
}

struct FileHandleWrap {
  napi_env env = nullptr;
  napi_ref wrapper_ref = nullptr;
  napi_ref closing_promise_ref = nullptr;
  napi_ref onread_ref = nullptr;
  napi_deferred closing_deferred = nullptr;
  int32_t fd = -1;
  uint64_t bytes_read = 0;
  uint64_t bytes_written = 0;
  int64_t async_id = 0;
  bool closing = false;
  bool closed = false;
};

struct FileHandleCloseReq {
  napi_env env = nullptr;
  FileHandleWrap* wrap = nullptr;
  uv_fs_t req{};
};

enum class AsyncFsResultKind {
  kUndefined,
  kInt64,
  kFileHandle,
};

struct AsyncFsReq {
  napi_env env = nullptr;
  ReqKind req_kind = ReqKind::kNone;
  napi_ref req_ref = nullptr;
  napi_ref oncomplete_ref = nullptr;
  napi_deferred deferred = nullptr;
  uv_fs_t req{};
  AsyncFsResultKind result_kind = AsyncFsResultKind::kUndefined;
  const char* syscall = nullptr;
  std::string path_storage;
  napi_ref* hold_refs = nullptr;
  size_t hold_ref_count = 0;
  uv_buf_t* bufs = nullptr;
  size_t nbufs = 0;
};

void HoldFileHandleRef(FileHandleWrap* wrap) {
  if (wrap == nullptr || wrap->env == nullptr || wrap->wrapper_ref == nullptr) return;
  uint32_t ref_count = 0;
  (void)napi_reference_ref(wrap->env, wrap->wrapper_ref, &ref_count);
}

void ReleaseFileHandleRef(FileHandleWrap* wrap) {
  if (wrap == nullptr || wrap->env == nullptr || wrap->wrapper_ref == nullptr) return;
  uint32_t ref_count = 0;
  (void)napi_reference_unref(wrap->env, wrap->wrapper_ref, &ref_count);
}

napi_value CreateUvExceptionValue(napi_env env, int errorno, const char* syscall);

void DestroyAsyncFsReq(AsyncFsReq* async_req) {
  if (async_req == nullptr) return;
  napi_env env = async_req->env;
  if (env != nullptr) {
    ResetRef(env, &async_req->req_ref);
    ResetRef(env, &async_req->oncomplete_ref);
    if (async_req->hold_refs != nullptr) {
      for (size_t i = 0; i < async_req->hold_ref_count; ++i) {
        ResetRef(env, &async_req->hold_refs[i]);
      }
    }
  }
  delete[] async_req->hold_refs;
  delete[] async_req->bufs;
  uv_fs_req_cleanup(&async_req->req);
  delete async_req;
}

bool IsNullOrUndefined(napi_env env, napi_value value) {
  if (value == nullptr) return true;
  napi_valuetype type = napi_undefined;
  if (napi_typeof(env, value, &type) != napi_ok) return true;
  return type == napi_undefined || type == napi_null;
}

void ThrowInvalidFdType(napi_env env) {
  napi_throw_type_error(env, "ERR_INVALID_ARG_TYPE", "The \"fd\" argument must be of type number");
}

void ThrowFdOutOfRange(napi_env env, int32_t value) {
  std::string message =
      "The value of \"fd\" is out of range. It must be >= 0 && <= 2147483647. Received ";
  message += std::to_string(value);
  napi_throw_range_error(env, "ERR_OUT_OF_RANGE", message.c_str());
}

bool ValidateFdArg(napi_env env, napi_value value, int32_t* fd_out) {
  if (fd_out == nullptr) return false;
  *fd_out = -1;
  if (value == nullptr) {
    ThrowInvalidFdType(env);
    return false;
  }
  int32_t fd = 0;
  if (napi_get_value_int32(env, value, &fd) != napi_ok) {
    ThrowInvalidFdType(env);
    return false;
  }
  if (fd < 0) {
    ThrowFdOutOfRange(env, fd);
    return false;
  }
  *fd_out = fd;
  return true;
}

int64_t GetInt64OrDefault(napi_env env, napi_value value, int64_t fallback) {
  if (IsNullOrUndefined(env, value)) return fallback;
  int64_t out = fallback;
  if (napi_get_value_int64(env, value, &out) != napi_ok) return fallback;
  return out;
}

bool ExtractByteSpanForAsyncIo(napi_env env,
                               napi_value value,
                               size_t offset,
                               size_t length,
                               napi_value* hold_value,
                               uv_buf_t* out) {
  if (hold_value == nullptr || out == nullptr) return false;

  napi_value backing = value;
  void* raw = nullptr;
  size_t total_len = 0;

  bool is_buffer = false;
  if (backing != nullptr && napi_is_buffer(env, backing, &is_buffer) == napi_ok && is_buffer) {
    if (napi_get_buffer_info(env, backing, &raw, &total_len) != napi_ok) return false;
  } else {
    bool is_typed_array = false;
    if (backing != nullptr && napi_is_typedarray(env, backing, &is_typed_array) == napi_ok && is_typed_array) {
      napi_typedarray_type type = napi_uint8_array;
      size_t len = 0;
      napi_value ab = nullptr;
      size_t byte_offset = 0;
      if (napi_get_typedarray_info(env, backing, &type, &len, &raw, &ab, &byte_offset) != napi_ok) return false;
      total_len = len * TypedArrayElementSize(type);
    } else {
      bool is_dataview = false;
      if (backing != nullptr && napi_is_dataview(env, backing, &is_dataview) == napi_ok && is_dataview) {
        napi_value ab = nullptr;
        size_t byte_offset = 0;
        if (napi_get_dataview_info(env, backing, &total_len, &raw, &ab, &byte_offset) != napi_ok) return false;
      } else {
        backing = BufferFromValue(env, value, nullptr);
        if (backing == nullptr || IsUndefined(env, backing)) return false;
        if (napi_get_buffer_info(env, backing, &raw, &total_len) != napi_ok) return false;
      }
    }
  }

  if (raw == nullptr || offset > total_len) return false;
  if (length > total_len - offset) length = total_len - offset;

  *hold_value = backing;
  *out = uv_buf_init(static_cast<char*>(raw) + offset, static_cast<unsigned int>(length));
  return true;
}

napi_value MakeAsyncFsResultValue(AsyncFsReq* async_req, int64_t result) {
  if (async_req == nullptr || async_req->env == nullptr) return nullptr;
  napi_env env = async_req->env;
  switch (async_req->result_kind) {
    case AsyncFsResultKind::kUndefined:
      return Undefined(env);
    case AsyncFsResultKind::kInt64: {
      napi_value out = nullptr;
      napi_create_int64(env, result, &out);
      return out != nullptr ? out : Undefined(env);
    }
    case AsyncFsResultKind::kFileHandle: {
      FsBindingState* st = GetState(env);
      napi_value ctor = st == nullptr ? nullptr : GetRefValue(env, st->file_handle_ctor_ref);
      if (ctor == nullptr) return Undefined(env);
      napi_value fd_value = nullptr;
      napi_create_int32(env, static_cast<int32_t>(result), &fd_value);
      napi_value argv[1] = {fd_value};
      napi_value handle = nullptr;
      if (napi_new_instance(env, ctor, 1, argv, &handle) != napi_ok || handle == nullptr) {
        return Undefined(env);
      }
      return handle;
    }
  }
  return Undefined(env);
}

void FinishAsyncFsReq(AsyncFsReq* async_req, int result) {
  if (async_req == nullptr || async_req->env == nullptr) {
    DestroyAsyncFsReq(async_req);
    return;
  }

  napi_env env = async_req->env;
  napi_value err = nullptr;
  napi_value value = Undefined(env);
  if (result < 0) {
    err = CreateUvExceptionValue(env, result, async_req->syscall != nullptr ? async_req->syscall : "");
  } else {
    value = MakeAsyncFsResultValue(async_req, result);
  }

  if (async_req->req_kind == ReqKind::kPromise && async_req->deferred != nullptr) {
    if (result < 0) {
      (void)napi_reject_deferred(env, async_req->deferred, err != nullptr ? err : Undefined(env));
    } else {
      (void)napi_resolve_deferred(env, async_req->deferred, value != nullptr ? value : Undefined(env));
    }
    (void)UbiRunCallbackScopeCheckpoint(env);
    DestroyAsyncFsReq(async_req);
    return;
  }

  if (async_req->req_kind == ReqKind::kCallback) {
    napi_value req = GetRefValue(env, async_req->req_ref);
    napi_value oncomplete = GetRefValue(env, async_req->oncomplete_ref);
    if (req != nullptr) UntrackActiveRequest(env, req);
    if (req != nullptr && oncomplete != nullptr) {
      napi_value argv[2] = {Undefined(env), Undefined(env)};
      size_t argc = 1;
      if (result < 0) {
        argv[0] = err != nullptr ? err : Undefined(env);
      } else if (value != nullptr && !IsUndefined(env, value)) {
        argv[1] = value;
        argc = 2;
      }
      napi_value ignored = nullptr;
      (void)UbiMakeCallback(env, req, oncomplete, argc, argv, &ignored);
    }
  }

  DestroyAsyncFsReq(async_req);
}

void AfterAsyncFsReq(uv_fs_t* req) {
  auto* async_req = static_cast<AsyncFsReq*>(req != nullptr ? req->data : nullptr);
  if (async_req == nullptr) return;
  FinishAsyncFsReq(async_req, static_cast<int>(req->result));
}

AsyncFsReq* CreateAsyncFsReq(napi_env env,
                             ReqKind req_kind,
                             napi_value req,
                             napi_value oncomplete,
                             napi_value* promise_out) {
  if (promise_out != nullptr) *promise_out = nullptr;
  auto* async_req = new AsyncFsReq();
  async_req->env = env;
  async_req->req_kind = req_kind;
  async_req->req.data = async_req;

  if (req_kind == ReqKind::kPromise) {
    napi_value promise = nullptr;
    if (napi_create_promise(env, &async_req->deferred, &promise) != napi_ok || promise == nullptr) {
      delete async_req;
      return nullptr;
    }
    if (promise_out != nullptr) *promise_out = promise;
    return async_req;
  }

  if (req_kind == ReqKind::kCallback && req != nullptr && oncomplete != nullptr &&
      napi_create_reference(env, req, 1, &async_req->req_ref) == napi_ok &&
      napi_create_reference(env, oncomplete, 1, &async_req->oncomplete_ref) == napi_ok) {
    TrackActiveRequest(env, req);
    return async_req;
  }

  delete async_req;
  return nullptr;
}

napi_value CreateUvExceptionValue(napi_env env, int errorno, const char* syscall) {
  const char* code = uv_err_name(errorno);
  const char* message = uv_strerror(errorno);
  std::string full_message;
  if (syscall != nullptr && *syscall != '\0') {
    full_message.append(syscall);
    full_message.push_back(' ');
  }
  full_message.append(message != nullptr ? message : "Unknown system error");

  napi_value message_value = nullptr;
  napi_value error = nullptr;
  napi_create_string_utf8(env, full_message.c_str(), NAPI_AUTO_LENGTH, &message_value);
  napi_create_error(env, nullptr, message_value, &error);
  if (error == nullptr) return Undefined(env);

  napi_value errno_value = nullptr;
  napi_create_int32(env, errorno, &errno_value);
  napi_set_named_property(env, error, "errno", errno_value);

  napi_value code_value = nullptr;
  napi_create_string_utf8(env, code != nullptr ? code : "UV_UNKNOWN", NAPI_AUTO_LENGTH, &code_value);
  napi_set_named_property(env, error, "code", code_value);

  if (syscall != nullptr && *syscall != '\0') {
    napi_value syscall_value = nullptr;
    napi_create_string_utf8(env, syscall, NAPI_AUTO_LENGTH, &syscall_value);
    napi_set_named_property(env, error, "syscall", syscall_value);
  }

  return error;
}

void FinishFileHandleClose(FileHandleCloseReq* close_req, int result) {
  if (close_req == nullptr) return;
  FileHandleWrap* wrap = close_req->wrap;
  napi_env env = close_req->env;

  if (wrap != nullptr) {
    wrap->closing = false;
    if (result >= 0) {
      wrap->closed = true;
      wrap->fd = -1;
    }
  }

  if (env != nullptr && wrap != nullptr && wrap->closing_deferred != nullptr) {
    if (result < 0) {
      napi_value err = CreateUvExceptionValue(env, result, "close");
      (void)napi_reject_deferred(env, wrap->closing_deferred, err);
    } else {
      (void)napi_resolve_deferred(env, wrap->closing_deferred, Undefined(env));
    }
    wrap->closing_deferred = nullptr;
    (void)UbiRunCallbackScopeCheckpoint(env);
  }

  if (env != nullptr && wrap != nullptr) {
    ResetRef(env, &wrap->closing_promise_ref);
    ReleaseFileHandleRef(wrap);
  }

  uv_fs_req_cleanup(&close_req->req);
  delete close_req;
}

void AfterFileHandleClose(uv_fs_t* req) {
  auto* close_req = static_cast<FileHandleCloseReq*>(req != nullptr ? req->data : nullptr);
  if (close_req == nullptr) return;
  FinishFileHandleClose(close_req, static_cast<int>(req->result));
}

FileHandleWrap* UnwrapFileHandle(napi_env env, napi_value this_arg) {
  if (this_arg == nullptr) return nullptr;
  void* data = nullptr;
  if (napi_unwrap(env, this_arg, &data) != napi_ok || data == nullptr) return nullptr;
  return static_cast<FileHandleWrap*>(data);
}

void FileHandleFinalize(napi_env env, void* data, void* /*hint*/) {
  auto* wrap = static_cast<FileHandleWrap*>(data);
  if (wrap == nullptr) return;
  ResetRef(env, &wrap->closing_promise_ref);
  ResetRef(env, &wrap->onread_ref);
  ResetRef(env, &wrap->wrapper_ref);
  delete wrap;
}

napi_value FileHandleCtor(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  napi_value this_arg = nullptr;
  napi_get_cb_info(env, info, &argc, argv, &this_arg, nullptr);
  if (this_arg == nullptr) return nullptr;
  auto* wrap = new FileHandleWrap();
  wrap->env = env;
  wrap->async_id = g_next_file_handle_async_id++;
  if (argc >= 1 && argv[0] != nullptr) napi_get_value_int32(env, argv[0], &wrap->fd);
  napi_wrap(env, this_arg, wrap, FileHandleFinalize, nullptr, &wrap->wrapper_ref);
  return this_arg;
}

napi_value FileHandleGetFd(napi_env env, napi_callback_info info) {
  napi_value this_arg = nullptr;
  size_t argc = 0;
  napi_get_cb_info(env, info, &argc, nullptr, &this_arg, nullptr);
  FileHandleWrap* wrap = UnwrapFileHandle(env, this_arg);
  napi_value out = nullptr;
  napi_create_int32(env, wrap == nullptr ? -1 : wrap->fd, &out);
  return out != nullptr ? out : Undefined(env);
}

napi_value FileHandleGetBytesRead(napi_env env, napi_callback_info info) {
  napi_value this_arg = nullptr;
  size_t argc = 0;
  napi_get_cb_info(env, info, &argc, nullptr, &this_arg, nullptr);
  FileHandleWrap* wrap = UnwrapFileHandle(env, this_arg);
  napi_value out = nullptr;
  napi_create_double(env, wrap == nullptr ? 0 : static_cast<double>(wrap->bytes_read), &out);
  return out != nullptr ? out : Undefined(env);
}

napi_value FileHandleGetBytesWritten(napi_env env, napi_callback_info info) {
  napi_value this_arg = nullptr;
  size_t argc = 0;
  napi_get_cb_info(env, info, &argc, nullptr, &this_arg, nullptr);
  FileHandleWrap* wrap = UnwrapFileHandle(env, this_arg);
  napi_value out = nullptr;
  napi_create_double(env, wrap == nullptr ? 0 : static_cast<double>(wrap->bytes_written), &out);
  return out != nullptr ? out : Undefined(env);
}

napi_value FileHandleGetExternalStream(napi_env env, napi_callback_info info) {
  napi_value this_arg = nullptr;
  size_t argc = 0;
  napi_get_cb_info(env, info, &argc, nullptr, &this_arg, nullptr);
  FileHandleWrap* wrap = UnwrapFileHandle(env, this_arg);
  napi_value out = nullptr;
  if (wrap != nullptr) {
    napi_create_external(env, wrap, nullptr, nullptr, &out);
  }
  return out != nullptr ? out : Undefined(env);
}

napi_value FileHandleGetAsyncId(napi_env env, napi_callback_info info) {
  napi_value this_arg = nullptr;
  size_t argc = 0;
  napi_get_cb_info(env, info, &argc, nullptr, &this_arg, nullptr);
  FileHandleWrap* wrap = UnwrapFileHandle(env, this_arg);
  napi_value out = nullptr;
  napi_create_int64(env, wrap == nullptr ? -1 : wrap->async_id, &out);
  return out != nullptr ? out : Undefined(env);
}

napi_value FileHandleGetOnread(napi_env env, napi_callback_info info) {
  napi_value this_arg = nullptr;
  size_t argc = 0;
  napi_get_cb_info(env, info, &argc, nullptr, &this_arg, nullptr);
  FileHandleWrap* wrap = UnwrapFileHandle(env, this_arg);
  napi_value value = wrap == nullptr ? nullptr : GetRefValue(env, wrap->onread_ref);
  return value != nullptr ? value : Undefined(env);
}

napi_value FileHandleSetOnread(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  napi_value this_arg = nullptr;
  napi_get_cb_info(env, info, &argc, argv, &this_arg, nullptr);
  FileHandleWrap* wrap = UnwrapFileHandle(env, this_arg);
  if (wrap == nullptr) return Undefined(env);
  ResetRef(env, &wrap->onread_ref);
  if (argc >= 1 && argv[0] != nullptr && !IsUndefined(env, argv[0])) {
    napi_create_reference(env, argv[0], 1, &wrap->onread_ref);
  }
  return Undefined(env);
}

napi_value FileHandleClose(napi_env env, napi_callback_info info) {
  size_t argc = 0;
  napi_value this_arg = nullptr;
  napi_get_cb_info(env, info, &argc, nullptr, &this_arg, nullptr);
  FileHandleWrap* wrap = UnwrapFileHandle(env, this_arg);
  if (wrap == nullptr) return Undefined(env);

  if (wrap->fd < 0 || wrap->closed) return MakeResolvedPromise(env, Undefined(env));

  napi_value closing_promise = GetRefValue(env, wrap->closing_promise_ref);
  if (closing_promise != nullptr && wrap->closing) return closing_promise;

  napi_deferred deferred = nullptr;
  napi_value promise = nullptr;
  if (napi_create_promise(env, &deferred, &promise) != napi_ok || promise == nullptr) return Undefined(env);

  napi_create_reference(env, promise, 1, &wrap->closing_promise_ref);
  wrap->closing_deferred = deferred;
  wrap->closing = true;

  auto* close_req = new FileHandleCloseReq();
  close_req->env = env;
  close_req->wrap = wrap;
  close_req->req.data = close_req;
  HoldFileHandleRef(wrap);

  const int rc = uv_fs_close(uv_default_loop(), &close_req->req, wrap->fd, AfterFileHandleClose);
  if (rc < 0) {
    FinishFileHandleClose(close_req, rc);
  }

  return promise;
}

napi_value FileHandleReleaseFD(napi_env env, napi_callback_info info) {
  napi_value this_arg = nullptr;
  size_t argc = 0;
  napi_get_cb_info(env, info, &argc, nullptr, &this_arg, nullptr);
  FileHandleWrap* wrap = UnwrapFileHandle(env, this_arg);
  int32_t old_fd = wrap == nullptr ? -1 : wrap->fd;
  if (wrap != nullptr) {
    wrap->fd = -1;
    wrap->closing = false;
    wrap->closed = true;
  }
  napi_value out = nullptr;
  napi_create_int32(env, old_fd, &out);
  return out != nullptr ? out : Undefined(env);
}

napi_value FileHandleReadStart(napi_env env, napi_callback_info info) {
  napi_value out = nullptr;
  napi_create_int32(env, 0, &out);
  return out != nullptr ? out : Undefined(env);
}

napi_value FileHandleReadStop(napi_env env, napi_callback_info info) {
  napi_value out = nullptr;
  napi_create_int32(env, 0, &out);
  return out != nullptr ? out : Undefined(env);
}

napi_value FileHandleShutdown(napi_env env, napi_callback_info info) {
  return Undefined(env);
}

napi_value FileHandleUseUserBuffer(napi_env env, napi_callback_info info) {
  napi_value out = nullptr;
  napi_get_boolean(env, false, &out);
  return out != nullptr ? out : Undefined(env);
}

napi_value CallBindingMethodByName(napi_env env, napi_value binding, const char* name, size_t argc, napi_value* argv) {
  napi_value fn = nullptr;
  napi_valuetype t = napi_undefined;
  if (napi_get_named_property(env, binding, name, &fn) != napi_ok ||
      fn == nullptr ||
      napi_typeof(env, fn, &t) != napi_ok ||
      t != napi_function) {
    return Undefined(env);
  }
  napi_value out = nullptr;
  if (napi_call_function(env, binding, fn, argc, argv, &out) != napi_ok || out == nullptr) return Undefined(env);
  return out;
}

napi_value FileHandleWritev(napi_env env, napi_callback_info info) {
  size_t argc = 3;
  napi_value argv[3] = {nullptr, nullptr, nullptr};
  napi_value this_arg = nullptr;
  napi_get_cb_info(env, info, &argc, argv, &this_arg, nullptr);
  FileHandleWrap* wrap = UnwrapFileHandle(env, this_arg);
  if (wrap == nullptr) return Undefined(env);
  napi_value binding = GetBinding(env);
  if (binding == nullptr) return Undefined(env);
  napi_value fd_value = nullptr;
  napi_create_int32(env, wrap->fd, &fd_value);
  napi_value call_argv[4] = {fd_value, argc >= 1 ? argv[0] : Undefined(env), argc >= 2 ? argv[1] : Undefined(env),
                             argc >= 3 ? argv[2] : Undefined(env)};
  return CallBindingMethodByName(env, binding, "writeBuffers", 4, call_argv);
}

napi_value FileHandleWriteBuffer(napi_env env, napi_callback_info info) {
  size_t argc = 5;
  napi_value argv[5] = {nullptr, nullptr, nullptr, nullptr, nullptr};
  napi_value this_arg = nullptr;
  napi_get_cb_info(env, info, &argc, argv, &this_arg, nullptr);
  FileHandleWrap* wrap = UnwrapFileHandle(env, this_arg);
  if (wrap == nullptr) return Undefined(env);
  napi_value binding = GetBinding(env);
  if (binding == nullptr) return Undefined(env);
  napi_value fd_value = nullptr;
  napi_create_int32(env, wrap->fd, &fd_value);
  napi_value call_argv[6] = {fd_value,
                             argc >= 1 ? argv[0] : Undefined(env),
                             argc >= 2 ? argv[1] : Undefined(env),
                             argc >= 3 ? argv[2] : Undefined(env),
                             argc >= 4 ? argv[3] : Undefined(env),
                             argc >= 5 ? argv[4] : Undefined(env)};
  return CallBindingMethodByName(env, binding, "writeBuffer", 6, call_argv);
}

napi_value FileHandleWriteStringWithEncoding(napi_env env,
                                             napi_callback_info info,
                                             const char* encoding) {
  size_t argc = 3;
  napi_value argv[3] = {nullptr, nullptr, nullptr};
  napi_value this_arg = nullptr;
  napi_get_cb_info(env, info, &argc, argv, &this_arg, nullptr);
  FileHandleWrap* wrap = UnwrapFileHandle(env, this_arg);
  if (wrap == nullptr) return Undefined(env);
  napi_value binding = GetBinding(env);
  if (binding == nullptr) return Undefined(env);
  napi_value fd_value = nullptr;
  napi_create_int32(env, wrap->fd, &fd_value);
  napi_value enc_value = nullptr;
  napi_create_string_utf8(env, encoding, NAPI_AUTO_LENGTH, &enc_value);
  napi_value call_argv[5] = {fd_value, argc >= 1 ? argv[0] : Undefined(env), argc >= 2 ? argv[1] : Undefined(env),
                             enc_value, argc >= 3 ? argv[2] : Undefined(env)};
  return CallBindingMethodByName(env, binding, "writeString", 5, call_argv);
}

napi_value FileHandleWriteAsciiString(napi_env env, napi_callback_info info) {
  return FileHandleWriteStringWithEncoding(env, info, "ascii");
}

napi_value FileHandleWriteUtf8String(napi_env env, napi_callback_info info) {
  return FileHandleWriteStringWithEncoding(env, info, "utf8");
}

napi_value FileHandleWriteUcs2String(napi_env env, napi_callback_info info) {
  return FileHandleWriteStringWithEncoding(env, info, "ucs2");
}

napi_value FileHandleWriteLatin1String(napi_env env, napi_callback_info info) {
  return FileHandleWriteStringWithEncoding(env, info, "latin1");
}

napi_value FileHandleIsStreamBase(napi_env env, napi_callback_info info) {
  napi_value out = nullptr;
  napi_get_boolean(env, true, &out);
  return out != nullptr ? out : Undefined(env);
}

struct StatWatcherWrap {
  napi_env env = nullptr;
  napi_ref wrapper_ref = nullptr;
  uv_fs_poll_t handle{};
  bool initialized = false;
  bool closing = false;
  bool closed = false;
  bool finalized = false;
  bool delete_on_close = false;
  bool referenced = true;
  bool use_bigint = false;
  int64_t async_id = 0;
};

StatWatcherWrap* UnwrapStatWatcher(napi_env env, napi_value this_arg) {
  if (env == nullptr || this_arg == nullptr) return nullptr;
  void* data = nullptr;
  if (napi_unwrap(env, this_arg, &data) != napi_ok || data == nullptr) return nullptr;
  return static_cast<StatWatcherWrap*>(data);
}

void OnStatWatcherClosed(uv_handle_t* handle) {
  auto* wrap = static_cast<StatWatcherWrap*>(handle != nullptr ? handle->data : nullptr);
  if (wrap == nullptr) return;
  wrap->closing = false;
  wrap->closed = true;
  wrap->initialized = false;
  if (wrap->finalized || wrap->delete_on_close) {
    ResetRef(wrap->env, &wrap->wrapper_ref);
    delete wrap;
  }
}

void CloseStatWatcher(StatWatcherWrap* wrap) {
  if (wrap == nullptr || wrap->closed || wrap->closing) return;
  if (!wrap->initialized) {
    wrap->closed = true;
    return;
  }
  wrap->closing = true;
  uv_close(reinterpret_cast<uv_handle_t*>(&wrap->handle), OnStatWatcherClosed);
}

void OnStatWatcherChange(uv_fs_poll_t* handle, int status, const uv_stat_t* prev, const uv_stat_t* curr) {
  auto* wrap = static_cast<StatWatcherWrap*>(handle != nullptr ? handle->data : nullptr);
  if (wrap == nullptr || wrap->env == nullptr) return;

  napi_value self = GetRefValue(wrap->env, wrap->wrapper_ref);
  if (self == nullptr) return;
  napi_value onchange = nullptr;
  napi_valuetype type = napi_undefined;
  if (napi_get_named_property(wrap->env, self, "onchange", &onchange) != napi_ok ||
      onchange == nullptr ||
      napi_typeof(wrap->env, onchange, &type) != napi_ok ||
      type != napi_function) {
    return;
  }

  uv_stat_t curr_copy{};
  uv_stat_t prev_copy{};
  if (curr != nullptr) curr_copy = *curr;
  if (prev != nullptr) prev_copy = *prev;

  napi_value argv[2] = {nullptr, nullptr};
  napi_create_int32(wrap->env, status, &argv[0]);
  argv[1] = CreateStatWatcherArray(wrap->env, wrap->use_bigint, &curr_copy, &prev_copy);

  napi_value ignored = nullptr;
  UbiMakeCallback(wrap->env, self, onchange, 2, argv, &ignored);
}

void StatWatcherFinalize(napi_env env, void* data, void* /*hint*/) {
  auto* wrap = static_cast<StatWatcherWrap*>(data);
  if (wrap == nullptr) return;
  wrap->finalized = true;
  if (!wrap->initialized || wrap->closed) {
    ResetRef(env, &wrap->wrapper_ref);
    delete wrap;
    return;
  }
  wrap->delete_on_close = true;
  CloseStatWatcher(wrap);
}

napi_value StatWatcherCtor(napi_env env, napi_callback_info info) {
  napi_value this_arg = nullptr;
  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  napi_get_cb_info(env, info, &argc, argv, &this_arg, nullptr);
  if (this_arg == nullptr) return nullptr;
  auto* wrap = new StatWatcherWrap();
  wrap->env = env;
  wrap->async_id = g_next_stat_watcher_async_id++;
  if (argc >= 1 && argv[0] != nullptr) {
    napi_get_value_bool(env, argv[0], &wrap->use_bigint);
  }
  if (napi_wrap(env, this_arg, wrap, StatWatcherFinalize, nullptr, &wrap->wrapper_ref) != napi_ok) {
    delete wrap;
    return nullptr;
  }
  return this_arg;
}

napi_value StatWatcherStart(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value argv[2] = {nullptr, nullptr};
  napi_value this_arg = nullptr;
  napi_get_cb_info(env, info, &argc, argv, &this_arg, nullptr);
  StatWatcherWrap* wrap = UnwrapStatWatcher(env, this_arg);
  if (wrap == nullptr || argc < 2) return MakeInt32(env, UV_EINVAL);
  if (wrap->initialized || wrap->closing || wrap->closed) return MakeInt32(env, 0);

  std::string path;
  if (!ValueToUtf8(env, argv[0], &path)) return MakeInt32(env, UV_EINVAL);
  uint32_t interval = 0;
  if (napi_get_value_uint32(env, argv[1], &interval) != napi_ok) return MakeInt32(env, UV_EINVAL);

  int rc = uv_fs_poll_init(uv_default_loop(), &wrap->handle);
  if (rc != 0) return MakeInt32(env, rc);

  wrap->handle.data = wrap;
  rc = uv_fs_poll_start(&wrap->handle, OnStatWatcherChange, path.c_str(), interval);
  if (rc != 0) {
    wrap->initialized = true;
    wrap->closing = true;
    uv_close(reinterpret_cast<uv_handle_t*>(&wrap->handle), OnStatWatcherClosed);
    return MakeInt32(env, rc);
  }

  wrap->initialized = true;
  wrap->closed = false;
  wrap->closing = false;
  wrap->referenced = true;
  return MakeInt32(env, 0);
}

napi_value StatWatcherClose(napi_env env, napi_callback_info info) {
  napi_value this_arg = nullptr;
  size_t argc = 0;
  napi_get_cb_info(env, info, &argc, nullptr, &this_arg, nullptr);
  StatWatcherWrap* wrap = UnwrapStatWatcher(env, this_arg);
  if (wrap != nullptr) CloseStatWatcher(wrap);
  return Undefined(env);
}

napi_value StatWatcherRef(napi_env env, napi_callback_info info) {
  napi_value this_arg = nullptr;
  size_t argc = 0;
  napi_get_cb_info(env, info, &argc, nullptr, &this_arg, nullptr);
  StatWatcherWrap* wrap = UnwrapStatWatcher(env, this_arg);
  if (wrap != nullptr && wrap->initialized && !wrap->closing && !wrap->closed && !wrap->referenced) {
    uv_ref(reinterpret_cast<uv_handle_t*>(&wrap->handle));
    wrap->referenced = true;
  }
  return Undefined(env);
}

napi_value StatWatcherUnref(napi_env env, napi_callback_info info) {
  napi_value this_arg = nullptr;
  size_t argc = 0;
  napi_get_cb_info(env, info, &argc, nullptr, &this_arg, nullptr);
  StatWatcherWrap* wrap = UnwrapStatWatcher(env, this_arg);
  if (wrap != nullptr && wrap->initialized && !wrap->closing && !wrap->closed && wrap->referenced) {
    uv_unref(reinterpret_cast<uv_handle_t*>(&wrap->handle));
    wrap->referenced = false;
  }
  return Undefined(env);
}

napi_value StatWatcherGetAsyncId(napi_env env, napi_callback_info info) {
  napi_value this_arg = nullptr;
  size_t argc = 0;
  napi_get_cb_info(env, info, &argc, nullptr, &this_arg, nullptr);
  StatWatcherWrap* wrap = UnwrapStatWatcher(env, this_arg);
  napi_value out = nullptr;
  napi_create_int64(env, wrap == nullptr ? -1 : wrap->async_id, &out);
  return out != nullptr ? out : Undefined(env);
}

napi_value FSReqCallbackCtor(napi_env env, napi_callback_info info) {
  napi_value this_arg = nullptr;
  size_t argc = 0;
  napi_get_cb_info(env, info, &argc, nullptr, &this_arg, nullptr);
  return this_arg != nullptr ? this_arg : Undefined(env);
}

napi_value FsAccess(napi_env env, napi_callback_info info) {
  size_t argc = 3;
  napi_value argv[3] = {nullptr, nullptr, nullptr};
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  napi_value req = argc >= 3 ? argv[2] : nullptr;
  napi_value oncomplete = nullptr;
  ReqKind req_kind = ParseReq(env, req, &oncomplete);

  napi_value call_argv[2] = {argc >= 1 ? argv[0] : Undefined(env), argc >= 2 ? argv[1] : Undefined(env)};
  napi_value ignored = nullptr;
  napi_value err = nullptr;
  if (!CallRaw(env, "accessSync", 2, call_argv, &ignored, &err)) {
    // Prefer raw async access when available, but do not erase sync failure
    // semantics when only accessSync exists.
    FsBindingState* st = GetState(env);
    const bool has_async_access =
        st != nullptr && st->raw_methods.find("access") != st->raw_methods.end();
    if (has_async_access) {
      napi_value async_err = nullptr;
      if (CallRaw(env, "access", 2, call_argv, &ignored, &async_err)) {
        if (req_kind == ReqKind::kPromise) return MakeResolvedPromise(env, Undefined(env));
        CompleteReq(env, req_kind, req, oncomplete, nullptr, Undefined(env));
        return Undefined(env);
      }
      if (async_err != nullptr) {
        err = async_err;
      }
    }
    if (req_kind == ReqKind::kPromise) return MakeRejectedPromise(env, err);
    CompleteReq(env, req_kind, req, oncomplete, err, Undefined(env));
    if (req_kind == ReqKind::kCallback) return Undefined(env);
    if (err != nullptr) {
      napi_throw(env, err);
      return nullptr;
    }
    return Undefined(env);
  }
  if (req_kind == ReqKind::kPromise) return MakeResolvedPromise(env, Undefined(env));
  CompleteReq(env, req_kind, req, oncomplete, nullptr, Undefined(env));
  return Undefined(env);
}

napi_value FsStatCommon(napi_env env,
                        napi_callback_info info,
                        const char* raw_name,
                        bool allow_throw_if_no_entry,
                        size_t stats_len) {
  size_t argc = 4;
  napi_value argv[4] = {nullptr, nullptr, nullptr, nullptr};
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);

  bool use_bigint = false;
  if (argc >= 2 && argv[1] != nullptr) napi_get_value_bool(env, argv[1], &use_bigint);
  napi_value req = argc >= 3 ? argv[2] : nullptr;
  bool throw_if_no_entry = true;
  if (allow_throw_if_no_entry && argc >= 4 && argv[3] != nullptr) {
    napi_get_value_bool(env, argv[3], &throw_if_no_entry);
  }
  napi_value oncomplete = nullptr;
  ReqKind req_kind = ParseReq(env, req, &oncomplete);

  napi_value call_argv[1] = {argc >= 1 ? argv[0] : Undefined(env)};
  napi_value raw_out = nullptr;
  napi_value err = nullptr;
  if (!CallRaw(env, raw_name, 1, call_argv, &raw_out, &err)) {
    if (!throw_if_no_entry &&
        (ErrorCodeEquals(env, err, "ENOENT") || ErrorCodeEquals(env, err, "ENOTDIR"))) {
      if (req_kind == ReqKind::kPromise) return MakeResolvedPromise(env, Undefined(env));
      CompleteReq(env, req_kind, req, oncomplete, nullptr, Undefined(env));
      return Undefined(env);
    }
    if (req_kind == ReqKind::kPromise) return MakeRejectedPromise(env, err);
    CompleteReq(env, req_kind, req, oncomplete, err, Undefined(env));
    if (req_kind == ReqKind::kCallback) return Undefined(env);
    if (err != nullptr) {
      napi_throw(env, err);
      return nullptr;
    }
    return Undefined(env);
  }

  napi_value typed = CreateTypedStatsArray(env, stats_len, use_bigint, raw_out);
  if (req_kind == ReqKind::kPromise) return MakeResolvedPromise(env, typed);
  CompleteReq(env, req_kind, req, oncomplete, nullptr, typed);
  return typed;
}

napi_value FsStat(napi_env env, napi_callback_info info) {
  return FsStatCommon(env, info, "stat", true, kFsStatsLength);
}

napi_value FsLstat(napi_env env, napi_callback_info info) {
  return FsStatCommon(env, info, "lstat", true, kFsStatsLength);
}

napi_value FsFstat(napi_env env, napi_callback_info info) {
  size_t argc = 4;
  napi_value argv[4] = {nullptr, nullptr, nullptr, nullptr};
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  int32_t fd = -1;
  if (!ValidateFdArg(env, argc >= 1 ? argv[0] : nullptr, &fd)) return nullptr;
  return FsStatCommon(env, info, "fstat", false, kFsStatsLength);
}

napi_value FsStatfs(napi_env env, napi_callback_info info) {
  size_t argc = 3;
  napi_value argv[3] = {nullptr, nullptr, nullptr};
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  bool use_bigint = false;
  if (argc >= 2 && argv[1] != nullptr) napi_get_value_bool(env, argv[1], &use_bigint);
  napi_value req = argc >= 3 ? argv[2] : nullptr;
  napi_value oncomplete = nullptr;
  ReqKind req_kind = ParseReq(env, req, &oncomplete);

  napi_value call_argv[1] = {argc >= 1 ? argv[0] : Undefined(env)};
  napi_value raw_out = nullptr;
  napi_value err = nullptr;
  if (!CallRaw(env, "statfs", 1, call_argv, &raw_out, &err)) {
    // Node exposes statfs even if platform support is partial; return zeroed stats
    // for environments without a native statfs implementation.
    napi_value typed = CreateTypedStatsArray(env, kFsStatFsLength, use_bigint, nullptr);
    if (req_kind == ReqKind::kPromise) return MakeResolvedPromise(env, typed);
    CompleteReq(env, req_kind, req, oncomplete, nullptr, typed);
    return typed;
  }
  napi_value typed = CreateTypedStatsArray(env, kFsStatFsLength, use_bigint, raw_out);
  if (req_kind == ReqKind::kPromise) return MakeResolvedPromise(env, typed);
  CompleteReq(env, req_kind, req, oncomplete, nullptr, typed);
  return typed;
}

napi_value FsReaddir(napi_env env, napi_callback_info info) {
  size_t argc = 4;
  napi_value argv[4] = {nullptr, nullptr, nullptr, nullptr};
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);

  const bool as_buffer = argc >= 2 && argv[1] != nullptr && IsBufferEncoding(env, argv[1]);
  bool with_file_types = false;
  if (argc >= 3 && argv[2] != nullptr) napi_get_value_bool(env, argv[2], &with_file_types);
  napi_value req = argc >= 4 ? argv[3] : nullptr;
  napi_value oncomplete = nullptr;
  ReqKind req_kind = ParseReq(env, req, &oncomplete);

  napi_value with_file_types_value = nullptr;
  napi_get_boolean(env, with_file_types, &with_file_types_value);
  napi_value call_argv[2] = {argc >= 1 ? argv[0] : Undefined(env),
                             with_file_types_value != nullptr ? with_file_types_value : Undefined(env)};
  napi_value raw_out = nullptr;
  napi_value err = nullptr;
  if (!CallRaw(env, "readdir", 2, call_argv, &raw_out, &err)) {
    if (req_kind == ReqKind::kPromise) return MakeRejectedPromise(env, err);
    CompleteReq(env, req_kind, req, oncomplete, err, Undefined(env));
    if (req_kind == ReqKind::kNone && err != nullptr) {
      napi_throw(env, err);
      return nullptr;
    }
    return Undefined(env);
  }

  napi_value out = raw_out;
  if (with_file_types) {
    napi_value names = nullptr;
    napi_value types = nullptr;
    if (raw_out != nullptr &&
        napi_get_element(env, raw_out, 0, &names) == napi_ok &&
        napi_get_element(env, raw_out, 1, &types) == napi_ok) {
      napi_value encoded_names = ConvertNameArrayToEncoding(env, names, as_buffer);
      napi_value pair = nullptr;
      if (napi_create_array_with_length(env, 2, &pair) == napi_ok && pair != nullptr) {
        napi_set_element(env, pair, 0, encoded_names != nullptr ? encoded_names : names);
        napi_set_element(env, pair, 1, types != nullptr ? types : Undefined(env));
        out = pair;
      }
    }
  } else {
    out = ConvertNameArrayToEncoding(env, raw_out, as_buffer);
  }

  if (req_kind == ReqKind::kPromise) return MakeResolvedPromise(env, out);
  CompleteReq(env, req_kind, req, oncomplete, nullptr, out);
  return out;
}

napi_value FsMkdtemp(napi_env env, napi_callback_info info) {
  size_t argc = 3;
  napi_value argv[3] = {nullptr, nullptr, nullptr};
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);

  const bool as_buffer = argc >= 2 && argv[1] != nullptr && IsBufferEncoding(env, argv[1]);
  napi_value req = argc >= 3 ? argv[2] : nullptr;
  napi_value oncomplete = nullptr;
  ReqKind req_kind = ParseReq(env, req, &oncomplete);

  napi_value call_argv[1] = {argc >= 1 ? argv[0] : Undefined(env)};
  napi_value raw_out = nullptr;
  napi_value err = nullptr;
  if (!CallRaw(env, "mkdtemp", 1, call_argv, &raw_out, &err)) {
    if (req_kind == ReqKind::kPromise) return MakeRejectedPromise(env, err);
    CompleteReq(env, req_kind, req, oncomplete, err, Undefined(env));
    if (req_kind == ReqKind::kNone && err != nullptr) {
      napi_throw(env, err);
      return nullptr;
    }
    return Undefined(env);
  }

  napi_value out = as_buffer ? BufferFromValue(env, raw_out, "utf8") : raw_out;
  if (out == nullptr || IsUndefined(env, out)) out = raw_out;
  if (req_kind == ReqKind::kPromise) return MakeResolvedPromise(env, out);
  CompleteReq(env, req_kind, req, oncomplete, nullptr, out);
  return out;
}

napi_value FsMkdir(napi_env env, napi_callback_info info) {
  size_t argc = 4;
  napi_value argv[4] = {nullptr, nullptr, nullptr, nullptr};
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);

  napi_value req = argc >= 4 ? argv[3] : nullptr;
  napi_value oncomplete = nullptr;
  ReqKind req_kind = ParseReq(env, req, &oncomplete);

  napi_value call_argv[3] = {argc >= 1 ? argv[0] : Undefined(env),
                             argc >= 2 ? argv[1] : Undefined(env),
                             argc >= 3 ? argv[2] : Undefined(env)};
  napi_value out = nullptr;
  napi_value err = nullptr;
  if (!CallRaw(env, "mkdir", 3, call_argv, &out, &err)) {
    if (req_kind == ReqKind::kPromise) return MakeRejectedPromise(env, err);
    CompleteReq(env, req_kind, req, oncomplete, err, Undefined(env));
    if (req_kind == ReqKind::kCallback) return Undefined(env);
    if (err != nullptr) {
      napi_throw(env, err);
      return nullptr;
    }
    return Undefined(env);
  }

  if (req_kind == ReqKind::kPromise) return MakeResolvedPromise(env, out != nullptr ? out : Undefined(env));
  if (req_kind == ReqKind::kCallback) {
    CompleteReq(env, req_kind, req, oncomplete, nullptr, out != nullptr ? out : Undefined(env));
    return Undefined(env);
  }
  return out != nullptr ? out : Undefined(env);
}

napi_value FsRename(napi_env env, napi_callback_info info) {
  size_t argc = 3;
  napi_value argv[3] = {nullptr, nullptr, nullptr};
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  napi_value req = argc >= 3 ? argv[2] : nullptr;
  napi_value oncomplete = nullptr;
  ReqKind req_kind = ParseReq(env, req, &oncomplete);
  napi_value call_argv[2] = {argc >= 1 ? argv[0] : Undefined(env), argc >= 2 ? argv[1] : Undefined(env)};
  return CompleteVoidRawFsMethod(env, "rename", req_kind, req, oncomplete, 2, call_argv);
}

napi_value FsCopyFile(napi_env env, napi_callback_info info) {
  size_t argc = 4;
  napi_value argv[4] = {nullptr, nullptr, nullptr, nullptr};
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  napi_value req = argc >= 4 ? argv[3] : nullptr;
  napi_value oncomplete = nullptr;
  ReqKind req_kind = ParseReq(env, req, &oncomplete);
  napi_value call_argv[3] = {argc >= 1 ? argv[0] : Undefined(env),
                             argc >= 2 ? argv[1] : Undefined(env),
                             argc >= 3 ? argv[2] : Undefined(env)};
  return CompleteVoidRawFsMethod(env, "copyFile", req_kind, req, oncomplete, 3, call_argv);
}

napi_value FsReadlink(napi_env env, napi_callback_info info) {
  size_t argc = 3;
  napi_value argv[3] = {nullptr, nullptr, nullptr};
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);

  napi_value req = argc >= 3 ? argv[2] : nullptr;
  napi_value oncomplete = nullptr;
  ReqKind req_kind = ParseReq(env, req, &oncomplete);

  std::string encoding = "utf8";
  if (argc >= 2 && argv[1] != nullptr) ValueToUtf8(env, argv[1], &encoding);

  napi_value call_argv[1] = {argc >= 1 ? argv[0] : Undefined(env)};
  napi_value raw_out = nullptr;
  napi_value err = nullptr;
  if (!CallRaw(env, "readlink", 1, call_argv, &raw_out, &err)) {
    if (req_kind == ReqKind::kPromise) return MakeRejectedPromise(env, err);
    CompleteReq(env, req_kind, req, oncomplete, err, Undefined(env));
    if (req_kind == ReqKind::kCallback) return Undefined(env);
    if (err != nullptr) {
      napi_throw(env, err);
      return nullptr;
    }
    return Undefined(env);
  }

  napi_value out = raw_out;
  if (encoding == "buffer") out = BufferFromValue(env, raw_out, nullptr);
  if (out == nullptr || IsUndefined(env, out)) out = raw_out;

  if (req_kind == ReqKind::kPromise) return MakeResolvedPromise(env, out);
  if (req_kind == ReqKind::kCallback) {
    CompleteReq(env, req_kind, req, oncomplete, nullptr, out);
    return Undefined(env);
  }
  return out != nullptr ? out : Undefined(env);
}

napi_value FsSymlink(napi_env env, napi_callback_info info) {
  size_t argc = 4;
  napi_value argv[4] = {nullptr, nullptr, nullptr, nullptr};
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  napi_value req = argc >= 4 ? argv[3] : nullptr;
  napi_value oncomplete = nullptr;
  ReqKind req_kind = ParseReq(env, req, &oncomplete);
  napi_value call_argv[3] = {argc >= 1 ? argv[0] : Undefined(env),
                             argc >= 2 ? argv[1] : Undefined(env),
                             argc >= 3 ? argv[2] : Undefined(env)};
  return CompleteVoidRawFsMethod(env, "symlink", req_kind, req, oncomplete, 3, call_argv);
}

napi_value FsUnlink(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value argv[2] = {nullptr, nullptr};
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  napi_value req = argc >= 2 ? argv[1] : nullptr;
  napi_value oncomplete = nullptr;
  ReqKind req_kind = ParseReq(env, req, &oncomplete);
  napi_value call_argv[1] = {argc >= 1 ? argv[0] : Undefined(env)};
  return CompleteVoidRawFsMethod(env, "unlink", req_kind, req, oncomplete, 1, call_argv);
}

napi_value FsRmdir(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value argv[2] = {nullptr, nullptr};
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  napi_value req = argc >= 2 ? argv[1] : nullptr;
  napi_value oncomplete = nullptr;
  ReqKind req_kind = ParseReq(env, req, &oncomplete);
  napi_value call_argv[1] = {argc >= 1 ? argv[0] : Undefined(env)};
  return CompleteVoidRawFsMethod(env, "rmdir", req_kind, req, oncomplete, 1, call_argv);
}

napi_value FsFtruncate(napi_env env, napi_callback_info info) {
  size_t argc = 3;
  napi_value argv[3] = {nullptr, nullptr, nullptr};
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  int32_t fd = -1;
  if (!ValidateFdArg(env, argc >= 1 ? argv[0] : nullptr, &fd)) return nullptr;
  napi_value req = argc >= 3 ? argv[2] : nullptr;
  napi_value oncomplete = nullptr;
  ReqKind req_kind = ParseReq(env, req, &oncomplete);
  napi_value call_argv[2] = {argc >= 1 ? argv[0] : Undefined(env), argc >= 2 ? argv[1] : Undefined(env)};
  return CompleteVoidRawFsMethod(env, "ftruncate", req_kind, req, oncomplete, 2, call_argv);
}

napi_value FsFsync(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value argv[2] = {nullptr, nullptr};
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  int32_t fd = -1;
  if (!ValidateFdArg(env, argc >= 1 ? argv[0] : nullptr, &fd)) return nullptr;
  napi_value req = argc >= 2 ? argv[1] : nullptr;
  napi_value oncomplete = nullptr;
  ReqKind req_kind = ParseReq(env, req, &oncomplete);
  napi_value call_argv[1] = {argc >= 1 ? argv[0] : Undefined(env)};
  return CompleteVoidRawFsMethod(env, "fsync", req_kind, req, oncomplete, 1, call_argv);
}

napi_value FsChmod(napi_env env, napi_callback_info info) {
  size_t argc = 3;
  napi_value argv[3] = {nullptr, nullptr, nullptr};
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  napi_value req = argc >= 3 ? argv[2] : nullptr;
  napi_value oncomplete = nullptr;
  ReqKind req_kind = ParseReq(env, req, &oncomplete);
  napi_value call_argv[2] = {argc >= 1 ? argv[0] : Undefined(env), argc >= 2 ? argv[1] : Undefined(env)};
  return CompleteVoidRawFsMethod(env, "chmod", req_kind, req, oncomplete, 2, call_argv);
}

napi_value FsFchmod(napi_env env, napi_callback_info info) {
  size_t argc = 3;
  napi_value argv[3] = {nullptr, nullptr, nullptr};
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  int32_t fd = -1;
  if (!ValidateFdArg(env, argc >= 1 ? argv[0] : nullptr, &fd)) return nullptr;
  napi_value req = argc >= 3 ? argv[2] : nullptr;
  napi_value oncomplete = nullptr;
  ReqKind req_kind = ParseReq(env, req, &oncomplete);
  napi_value call_argv[2] = {argc >= 1 ? argv[0] : Undefined(env), argc >= 2 ? argv[1] : Undefined(env)};
  return CompleteVoidRawFsMethod(env, "fchmod", req_kind, req, oncomplete, 2, call_argv);
}

napi_value FsUtimes(napi_env env, napi_callback_info info) {
  size_t argc = 4;
  napi_value argv[4] = {nullptr, nullptr, nullptr, nullptr};
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  napi_value req = argc >= 4 ? argv[3] : nullptr;
  napi_value oncomplete = nullptr;
  ReqKind req_kind = ParseReq(env, req, &oncomplete);
  napi_value call_argv[3] = {argc >= 1 ? argv[0] : Undefined(env),
                             argc >= 2 ? argv[1] : Undefined(env),
                             argc >= 3 ? argv[2] : Undefined(env)};
  return CompleteVoidRawFsMethod(env, "utimes", req_kind, req, oncomplete, 3, call_argv);
}

napi_value FsFutimes(napi_env env, napi_callback_info info) {
  size_t argc = 4;
  napi_value argv[4] = {nullptr, nullptr, nullptr, nullptr};
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  int32_t fd = -1;
  if (!ValidateFdArg(env, argc >= 1 ? argv[0] : nullptr, &fd)) return nullptr;
  napi_value req = argc >= 4 ? argv[3] : nullptr;
  napi_value oncomplete = nullptr;
  ReqKind req_kind = ParseReq(env, req, &oncomplete);
  napi_value call_argv[3] = {argc >= 1 ? argv[0] : Undefined(env),
                             argc >= 2 ? argv[1] : Undefined(env),
                             argc >= 3 ? argv[2] : Undefined(env)};
  return CompleteVoidRawFsMethod(env, "futimes", req_kind, req, oncomplete, 3, call_argv);
}

napi_value FsLutimes(napi_env env, napi_callback_info info) {
  size_t argc = 4;
  napi_value argv[4] = {nullptr, nullptr, nullptr, nullptr};
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  napi_value req = argc >= 4 ? argv[3] : nullptr;
  napi_value oncomplete = nullptr;
  ReqKind req_kind = ParseReq(env, req, &oncomplete);
  napi_value call_argv[3] = {argc >= 1 ? argv[0] : Undefined(env),
                             argc >= 2 ? argv[1] : Undefined(env),
                             argc >= 3 ? argv[2] : Undefined(env)};
  return CompleteVoidRawFsMethod(env, "lutimes", req_kind, req, oncomplete, 3, call_argv);
}

napi_value FsRead(napi_env env, napi_callback_info info) {
  size_t argc = 6;
  napi_value argv[6] = {nullptr, nullptr, nullptr, nullptr, nullptr, nullptr};
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  int32_t fd = -1;
  if (!ValidateFdArg(env, argc >= 1 ? argv[0] : nullptr, &fd)) return nullptr;
  napi_value req = argc >= 6 ? argv[5] : nullptr;
  napi_value oncomplete = nullptr;
  ReqKind req_kind = ParseReq(env, req, &oncomplete);

  napi_value call_argv[5] = {argc >= 1 ? argv[0] : Undefined(env), argc >= 2 ? argv[1] : Undefined(env),
                             argc >= 3 ? argv[2] : Undefined(env), argc >= 4 ? argv[3] : Undefined(env),
                             argc >= 5 ? argv[4] : Undefined(env)};
  napi_value out = nullptr;
  napi_value err = nullptr;
  if (!CallRaw(env, "readSync", 5, call_argv, &out, &err)) {
    if (req_kind == ReqKind::kPromise) return MakeRejectedPromise(env, err);
    CompleteReq(env, req_kind, req, oncomplete, err, Undefined(env));
    if (req_kind == ReqKind::kCallback) return Undefined(env);
    if (err != nullptr) {
      napi_throw(env, err);
      return nullptr;
    }
    return Undefined(env);
  }
  if (req_kind == ReqKind::kPromise) return MakeResolvedPromise(env, out);
  CompleteReq(env, req_kind, req, oncomplete, nullptr, out);
  return out;
}

napi_value FsOpen(napi_env env, napi_callback_info info) {
  size_t argc = 4;
  napi_value argv[4] = {nullptr, nullptr, nullptr, nullptr};
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);

  napi_value req = argc >= 4 ? argv[3] : nullptr;
  napi_value oncomplete = nullptr;
  ReqKind req_kind = ParseReq(env, req, &oncomplete);

  napi_value call_argv[3] = {argc >= 1 ? argv[0] : Undefined(env),
                             argc >= 2 ? argv[1] : Undefined(env),
                             argc >= 3 ? argv[2] : Undefined(env)};
  napi_value out = nullptr;
  napi_value err = nullptr;
  if (!CallRaw(env, "open", 3, call_argv, &out, &err)) {
    if (req_kind == ReqKind::kPromise) return MakeRejectedPromise(env, err);
    CompleteReq(env, req_kind, req, oncomplete, err, Undefined(env));
    if (req_kind == ReqKind::kCallback) return Undefined(env);
    if (err != nullptr) {
      napi_throw(env, err);
      return nullptr;
    }
    return Undefined(env);
  }

  if (req_kind == ReqKind::kPromise) return MakeResolvedPromise(env, out);
  if (req_kind == ReqKind::kCallback) {
    CompleteReq(env, req_kind, req, oncomplete, nullptr, out);
    return Undefined(env);
  }
  return out;
}

napi_value FsRealpath(napi_env env, napi_callback_info info) {
  size_t argc = 3;
  napi_value argv[3] = {nullptr, nullptr, nullptr};
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);

  napi_value req = argc >= 3 ? argv[2] : nullptr;
  napi_value oncomplete = nullptr;
  ReqKind req_kind = ParseReq(env, req, &oncomplete);

  std::string encoding = "utf8";
  if (argc >= 2 && argv[1] != nullptr) ValueToUtf8(env, argv[1], &encoding);

  napi_value call_argv[1] = {argc >= 1 ? argv[0] : Undefined(env)};
  napi_value raw_out = nullptr;
  napi_value err = nullptr;
  if (!CallRaw(env, "realpath", 1, call_argv, &raw_out, &err)) {
    if (req_kind == ReqKind::kPromise) return MakeRejectedPromise(env, err);
    CompleteReq(env, req_kind, req, oncomplete, err, Undefined(env));
    if (req_kind == ReqKind::kCallback) return Undefined(env);
    if (err != nullptr) {
      napi_throw(env, err);
      return nullptr;
    }
    return Undefined(env);
  }

  napi_value out = raw_out;
  if (encoding == "buffer") out = BufferFromValue(env, raw_out, nullptr);
  if (req_kind == ReqKind::kPromise) return MakeResolvedPromise(env, out);
  if (req_kind == ReqKind::kCallback) {
    CompleteReq(env, req_kind, req, oncomplete, nullptr, out);
    return Undefined(env);
  }
  return out != nullptr ? out : Undefined(env);
}

napi_value FsClose(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value argv[2] = {nullptr, nullptr};
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);

  napi_value req = argc >= 2 ? argv[1] : nullptr;
  napi_value oncomplete = nullptr;
  ReqKind req_kind = ParseReq(env, req, &oncomplete);

  napi_value call_argv[1] = {argc >= 1 ? argv[0] : Undefined(env)};
  napi_value out = nullptr;
  napi_value err = nullptr;
  if (!CallRaw(env, "close", 1, call_argv, &out, &err)) {
    if (req_kind == ReqKind::kPromise) return MakeRejectedPromise(env, err);
    CompleteReq(env, req_kind, req, oncomplete, err, Undefined(env));
    if (req_kind == ReqKind::kCallback) return Undefined(env);
    if (err != nullptr) {
      napi_throw(env, err);
      return nullptr;
    }
    return Undefined(env);
  }

  if (req_kind == ReqKind::kPromise) return MakeResolvedPromise(env, Undefined(env));
  if (req_kind == ReqKind::kCallback) {
    CompleteReq(env, req_kind, req, oncomplete, nullptr, Undefined(env));
    return Undefined(env);
  }
  return out != nullptr ? out : Undefined(env);
}

napi_value FsReadBuffers(napi_env env, napi_callback_info info) {
  size_t argc = 4;
  napi_value argv[4] = {nullptr, nullptr, nullptr, nullptr};
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  napi_value req = argc >= 4 ? argv[3] : nullptr;
  napi_value oncomplete = nullptr;
  ReqKind req_kind = ParseReq(env, req, &oncomplete);

  int64_t position = -1;
  if (argc >= 3 && argv[2] != nullptr) napi_get_value_int64(env, argv[2], &position);

  uint32_t len = 0;
  bool is_array = false;
  if (argc < 2 || napi_is_array(env, argv[1], &is_array) != napi_ok || !is_array ||
      napi_get_array_length(env, argv[1], &len) != napi_ok) {
    return Undefined(env);
  }

  int64_t total = 0;
  for (uint32_t i = 0; i < len; ++i) {
    napi_value view = nullptr;
    if (napi_get_element(env, argv[1], i, &view) != napi_ok || view == nullptr) continue;
    const napi_value read_argv[5] = {argv[0], view, nullptr, nullptr, nullptr};
    napi_value offset = nullptr;
    napi_value length = nullptr;
    napi_create_uint32(env, 0, &offset);
    size_t chunk_len = ByteLengthOfValue(env, view);
    napi_create_uint32(env, static_cast<uint32_t>(chunk_len), &length);
    napi_value pos = nullptr;
    napi_create_int64(env, position, &pos);
    napi_value mutable_args[5] = {argv[0], view, offset, length, pos};
    napi_value chunk_out = nullptr;
    napi_value err = nullptr;
    if (!CallRaw(env, "readSync", 5, mutable_args, &chunk_out, &err)) {
      if (req_kind == ReqKind::kPromise) return MakeRejectedPromise(env, err);
      CompleteReq(env, req_kind, req, oncomplete, err, Undefined(env));
      if (req_kind == ReqKind::kCallback) return Undefined(env);
      if (err != nullptr) {
        napi_throw(env, err);
        return nullptr;
      }
      return Undefined(env);
    }
    int64_t n = 0;
    napi_get_value_int64(env, chunk_out, &n);
    total += n;
    if (position >= 0 && n > 0) position += n;
  }

  napi_value out = nullptr;
  napi_create_int64(env, total, &out);
  if (req_kind == ReqKind::kPromise) return MakeResolvedPromise(env, out);
  CompleteReq(env, req_kind, req, oncomplete, nullptr, out);
  return out;
}

napi_value FsWriteBuffer(napi_env env, napi_callback_info info) {
  size_t argc = 6;
  napi_value argv[6] = {nullptr, nullptr, nullptr, nullptr, nullptr, nullptr};
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  int32_t validated_fd = -1;
  if (!ValidateFdArg(env, argc >= 1 ? argv[0] : nullptr, &validated_fd)) return nullptr;
  napi_value req = argc >= 6 ? argv[5] : nullptr;
  napi_value oncomplete = nullptr;
  ReqKind req_kind = ParseReq(env, req, &oncomplete);

  if (req_kind != ReqKind::kNone) {
    int32_t fd = validated_fd;
    size_t offset = 0;
    if (argc >= 3 && argv[2] != nullptr) {
      uint32_t offset_u32 = 0;
      napi_get_value_uint32(env, argv[2], &offset_u32);
      offset = offset_u32;
    }
    size_t length = ByteLengthOfValue(env, argc >= 2 ? argv[1] : nullptr);
    if (argc >= 4 && argv[3] != nullptr) {
      uint32_t length_u32 = 0;
      napi_get_value_uint32(env, argv[3], &length_u32);
      length = length_u32;
    } else if (length >= offset) {
      length -= offset;
    }
    const int64_t position = GetInt64OrDefault(env, argc >= 5 ? argv[4] : nullptr, -1);

    napi_value promise = nullptr;
    AsyncFsReq* async_req = CreateAsyncFsReq(env, req_kind, req, oncomplete, &promise);
    if (async_req != nullptr) {
      async_req->syscall = "write";
      async_req->result_kind = AsyncFsResultKind::kInt64;
      async_req->hold_refs = new napi_ref[1]();
      async_req->hold_ref_count = 1;
      async_req->bufs = new uv_buf_t[1];
      async_req->nbufs = 1;

      napi_value hold_value = nullptr;
      if (ExtractByteSpanForAsyncIo(env, argc >= 2 ? argv[1] : nullptr, offset, length, &hold_value, &async_req->bufs[0]) &&
          hold_value != nullptr &&
          napi_create_reference(env, hold_value, 1, &async_req->hold_refs[0]) == napi_ok) {
        const int rc = uv_fs_write(uv_default_loop(),
                                   &async_req->req,
                                   fd,
                                   async_req->bufs,
                                   1,
                                   position,
                                   AfterAsyncFsReq);
        if (rc < 0) FinishAsyncFsReq(async_req, rc);
        return req_kind == ReqKind::kPromise ? promise : Undefined(env);
      }

      FinishAsyncFsReq(async_req, UV_EINVAL);
      return req_kind == ReqKind::kPromise ? promise : Undefined(env);
    }
  }

  napi_value call_argv[5] = {argc >= 1 ? argv[0] : Undefined(env), argc >= 2 ? argv[1] : Undefined(env),
                             argc >= 3 ? argv[2] : Undefined(env), argc >= 4 ? argv[3] : Undefined(env),
                             argc >= 5 ? argv[4] : Undefined(env)};
  napi_value out = nullptr;
  napi_value err = nullptr;
  if (!CallRaw(env, "writeSync", 5, call_argv, &out, &err)) {
    if (req_kind == ReqKind::kPromise) return MakeRejectedPromise(env, err);
    CompleteReq(env, req_kind, req, oncomplete, err, Undefined(env));
    if (req_kind == ReqKind::kCallback) return Undefined(env);
    if (err != nullptr) {
      napi_throw(env, err);
      return nullptr;
    }
    return Undefined(env);
  }
  if (req_kind == ReqKind::kPromise) return MakeResolvedPromise(env, out);
  CompleteReq(env, req_kind, req, oncomplete, nullptr, out);
  return out;
}

napi_value FsWriteString(napi_env env, napi_callback_info info) {
  size_t argc = 5;
  napi_value argv[5] = {nullptr, nullptr, nullptr, nullptr, nullptr};
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  int32_t validated_fd = -1;
  if (!ValidateFdArg(env, argc >= 1 ? argv[0] : nullptr, &validated_fd)) return nullptr;
  napi_value req = argc >= 5 ? argv[4] : nullptr;
  napi_value oncomplete = nullptr;
  ReqKind req_kind = ParseReq(env, req, &oncomplete);

  std::string encoding = "utf8";
  if (argc >= 4 && argv[3] != nullptr) ValueToUtf8(env, argv[3], &encoding);
  napi_value buffer = BufferFromValue(env, argc >= 2 ? argv[1] : Undefined(env), encoding.c_str());
  const size_t byte_length = ByteLengthOfValue(env, buffer);
  if (req_kind != ReqKind::kNone) {
    const int32_t fd = validated_fd;
    const int64_t position = GetInt64OrDefault(env, argc >= 3 ? argv[2] : nullptr, -1);

    napi_value promise = nullptr;
    AsyncFsReq* async_req = CreateAsyncFsReq(env, req_kind, req, oncomplete, &promise);
    if (async_req != nullptr) {
      async_req->syscall = "write";
      async_req->result_kind = AsyncFsResultKind::kInt64;
      async_req->hold_refs = new napi_ref[1]();
      async_req->hold_ref_count = 1;
      async_req->bufs = new uv_buf_t[1];
      async_req->nbufs = 1;

      napi_value hold_value = nullptr;
      if (ExtractByteSpanForAsyncIo(env, buffer, 0, byte_length, &hold_value, &async_req->bufs[0]) &&
          hold_value != nullptr &&
          napi_create_reference(env, hold_value, 1, &async_req->hold_refs[0]) == napi_ok) {
        const int rc = uv_fs_write(uv_default_loop(),
                                   &async_req->req,
                                   fd,
                                   async_req->bufs,
                                   1,
                                   position,
                                   AfterAsyncFsReq);
        if (rc < 0) FinishAsyncFsReq(async_req, rc);
        return req_kind == ReqKind::kPromise ? promise : Undefined(env);
      }

      FinishAsyncFsReq(async_req, UV_EINVAL);
      return req_kind == ReqKind::kPromise ? promise : Undefined(env);
    }
  }

  napi_value zero = nullptr;
  napi_create_uint32(env, 0, &zero);
  napi_value length = nullptr;
  napi_create_uint32(env, static_cast<uint32_t>(byte_length), &length);

  napi_value position = argc >= 3 ? argv[2] : Undefined(env);
  if (position == nullptr || IsUndefined(env, position)) {
    napi_create_int64(env, -1, &position);
  } else {
    napi_valuetype pt = napi_undefined;
    if (napi_typeof(env, position, &pt) == napi_ok && pt == napi_null) {
      napi_create_int64(env, -1, &position);
    }
  }

  napi_value call_argv[5] = {argc >= 1 ? argv[0] : Undefined(env), buffer, zero, length, position};
  napi_value out = nullptr;
  napi_value err = nullptr;
  if (!CallRaw(env, "writeSync", 5, call_argv, &out, &err)) {
    if (req_kind == ReqKind::kPromise) return MakeRejectedPromise(env, err);
    CompleteReq(env, req_kind, req, oncomplete, err, Undefined(env));
    if (req_kind == ReqKind::kCallback) return Undefined(env);
    if (err != nullptr) {
      napi_throw(env, err);
      return nullptr;
    }
    return Undefined(env);
  }
  if (req_kind == ReqKind::kPromise) return MakeResolvedPromise(env, out);
  CompleteReq(env, req_kind, req, oncomplete, nullptr, out);
  return out;
}

napi_value FsWriteBuffers(napi_env env, napi_callback_info info) {
  size_t argc = 4;
  napi_value argv[4] = {nullptr, nullptr, nullptr, nullptr};
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  int32_t validated_fd = -1;
  if (!ValidateFdArg(env, argc >= 1 ? argv[0] : nullptr, &validated_fd)) return nullptr;
  napi_value req = argc >= 4 ? argv[3] : nullptr;
  napi_value oncomplete = nullptr;
  ReqKind req_kind = ParseReq(env, req, &oncomplete);

  int64_t position = -1;
  if (argc >= 3 && argv[2] != nullptr) napi_get_value_int64(env, argv[2], &position);

  uint32_t len = 0;
  bool is_array = false;
  if (argc < 2 || napi_is_array(env, argv[1], &is_array) != napi_ok || !is_array ||
      napi_get_array_length(env, argv[1], &len) != napi_ok) {
    return Undefined(env);
  }

  if (req_kind != ReqKind::kNone) {
    const int32_t fd = validated_fd;

    napi_value promise = nullptr;
    AsyncFsReq* async_req = CreateAsyncFsReq(env, req_kind, req, oncomplete, &promise);
    if (async_req != nullptr) {
      async_req->syscall = "write";
      async_req->result_kind = AsyncFsResultKind::kInt64;
      async_req->hold_refs = new napi_ref[len]();
      async_req->hold_ref_count = len;
      async_req->bufs = new uv_buf_t[len];
      async_req->nbufs = len;

      bool ok = true;
      for (uint32_t i = 0; i < len; ++i) {
        napi_value chunk = nullptr;
        if (napi_get_element(env, argv[1], i, &chunk) != napi_ok || chunk == nullptr) {
          ok = false;
          break;
        }
        napi_value hold_value = nullptr;
        const size_t byte_length = ByteLengthOfValue(env, chunk);
        if (!ExtractByteSpanForAsyncIo(env, chunk, 0, byte_length, &hold_value, &async_req->bufs[i]) ||
            hold_value == nullptr ||
            napi_create_reference(env, hold_value, 1, &async_req->hold_refs[i]) != napi_ok) {
          ok = false;
          break;
        }
      }

      if (ok) {
        const int rc = uv_fs_write(uv_default_loop(),
                                   &async_req->req,
                                   fd,
                                   async_req->bufs,
                                   len,
                                   position,
                                   AfterAsyncFsReq);
        if (rc < 0) FinishAsyncFsReq(async_req, rc);
        return req_kind == ReqKind::kPromise ? promise : Undefined(env);
      }

      FinishAsyncFsReq(async_req, UV_EINVAL);
      return req_kind == ReqKind::kPromise ? promise : Undefined(env);
    }
  }

  int64_t total = 0;
  for (uint32_t i = 0; i < len; ++i) {
    napi_value chunk = nullptr;
    if (napi_get_element(env, argv[1], i, &chunk) != napi_ok || chunk == nullptr) continue;
    napi_value buffer = BufferFromValue(env, chunk, nullptr);
    const size_t chunk_len = ByteLengthOfValue(env, buffer);
    napi_value zero = nullptr;
    napi_value length = nullptr;
    napi_value pos = nullptr;
    napi_create_uint32(env, 0, &zero);
    napi_create_uint32(env, static_cast<uint32_t>(chunk_len), &length);
    napi_create_int64(env, position, &pos);
    napi_value call_argv[5] = {argv[0], buffer, zero, length, pos};
    napi_value chunk_out = nullptr;
    napi_value err = nullptr;
    if (!CallRaw(env, "writeSync", 5, call_argv, &chunk_out, &err)) {
      if (req_kind == ReqKind::kPromise) return MakeRejectedPromise(env, err);
      CompleteReq(env, req_kind, req, oncomplete, err, Undefined(env));
      if (req_kind == ReqKind::kCallback) return Undefined(env);
      if (err != nullptr) {
        napi_throw(env, err);
        return nullptr;
      }
      return Undefined(env);
    }
    int64_t written = 0;
    napi_get_value_int64(env, chunk_out, &written);
    total += written;
    if (position >= 0 && written > 0) position += written;
  }

  napi_value out = nullptr;
  napi_create_int64(env, total, &out);
  if (req_kind == ReqKind::kPromise) return MakeResolvedPromise(env, out);
  CompleteReq(env, req_kind, req, oncomplete, nullptr, out);
  return out;
}

napi_value FsOpenFileHandle(napi_env env, napi_callback_info info) {
  size_t argc = 4;
  napi_value argv[4] = {nullptr, nullptr, nullptr, nullptr};
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  napi_value req = argc >= 4 ? argv[3] : nullptr;
  napi_value oncomplete = nullptr;
  ReqKind req_kind = ParseReq(env, req, &oncomplete);

  if (req_kind != ReqKind::kNone) {
    std::string path;
    int32_t flags = 0;
    int32_t mode = 0;
    if (!ValueToUtf8(env, argc >= 1 ? argv[0] : nullptr, &path)) {
      napi_value err = CreateUvExceptionValue(env, UV_EINVAL, "open");
      if (req_kind == ReqKind::kPromise) return MakeRejectedPromise(env, err);
      CompleteReq(env, req_kind, req, oncomplete, err, Undefined(env));
      return Undefined(env);
    }
    if (argc >= 2 && argv[1] != nullptr) napi_get_value_int32(env, argv[1], &flags);
    if (argc >= 3 && argv[2] != nullptr) napi_get_value_int32(env, argv[2], &mode);

    napi_value promise = nullptr;
    AsyncFsReq* async_req = CreateAsyncFsReq(env, req_kind, req, oncomplete, &promise);
    if (async_req != nullptr) {
      async_req->syscall = "open";
      async_req->result_kind = AsyncFsResultKind::kFileHandle;
      async_req->path_storage = std::move(path);
      const int rc = uv_fs_open(uv_default_loop(),
                                &async_req->req,
                                async_req->path_storage.c_str(),
                                flags,
                                mode,
                                AfterAsyncFsReq);
      if (rc < 0) FinishAsyncFsReq(async_req, rc);
      return req_kind == ReqKind::kPromise ? promise : Undefined(env);
    }
  }

  napi_value call_argv[3] = {argc >= 1 ? argv[0] : Undefined(env), argc >= 2 ? argv[1] : Undefined(env),
                             argc >= 3 ? argv[2] : Undefined(env)};
  napi_value fd_value = nullptr;
  napi_value err = nullptr;
  if (!CallRaw(env, "open", 3, call_argv, &fd_value, &err)) {
    if (req_kind == ReqKind::kPromise) return MakeRejectedPromise(env, err);
    CompleteReq(env, req_kind, req, oncomplete, err, Undefined(env));
    if (req_kind == ReqKind::kCallback) return Undefined(env);
    if (err != nullptr) {
      napi_throw(env, err);
      return nullptr;
    }
    return Undefined(env);
  }

  FsBindingState* st = GetState(env);
  napi_value ctor = st == nullptr ? nullptr : GetRefValue(env, st->file_handle_ctor_ref);
  if (ctor == nullptr) return Undefined(env);
  napi_value ctor_argv[1] = {fd_value};
  napi_value handle = nullptr;
  if (napi_new_instance(env, ctor, 1, ctor_argv, &handle) != napi_ok || handle == nullptr) {
    return Undefined(env);
  }

  if (req_kind == ReqKind::kPromise) return MakeResolvedPromise(env, handle);
  CompleteReq(env, req_kind, req, oncomplete, nullptr, handle);
  return handle;
}

napi_value FsInternalModuleStat(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  std::string path;
  if (argc < 1 || !ValueToUtf8(env, argv[0], &path)) return Undefined(env);

  std::error_code ec;
  const auto status = std::filesystem::status(path, ec);
  int32_t out_value = -1;
  if (!ec) {
    if (std::filesystem::is_directory(status)) out_value = 1;
    else if (std::filesystem::is_regular_file(status)) out_value = 0;
    else out_value = 0;
  } else {
    out_value = (ec == std::errc::no_such_file_or_directory || ec == std::errc::not_a_directory) ? -2 : -1;
  }
  napi_value out = nullptr;
  napi_create_int32(env, out_value, &out);
  return out != nullptr ? out : Undefined(env);
}

napi_value FsLegacyMainResolve(napi_env env, napi_callback_info info) {
  size_t argc = 3;
  napi_value argv[3] = {nullptr, nullptr, nullptr};
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  std::string package_path;
  if (argc < 1 || !ValueToUtf8(env, argv[0], &package_path)) return Undefined(env);

  if (argc >= 3 && argv[2] != nullptr) {
    napi_valuetype t = napi_undefined;
    if (napi_typeof(env, argv[2], &t) != napi_ok || (t != napi_string && t != napi_object)) {
      napi_throw_type_error(env,
                            "ERR_INVALID_ARG_TYPE",
                            "The \"base\" argument must be of type string or an instance of URL.");
      return nullptr;
    }
  }

  static const char* ext[] = {"", ".js", ".json", ".node", "/index.js", "/index.json", "/index.node",
                              ".js", ".json", ".node"};

  auto internal_module_stat = [&](const std::string& candidate) -> int32_t {
    std::error_code ec;
    const auto status = std::filesystem::symlink_status(candidate, ec);
    if (ec) return -2;
    return std::filesystem::is_directory(status) ? 1 : 0;
  };

  std::string package_main;
  if (argc >= 2 && argv[1] != nullptr) ValueToUtf8(env, argv[1], &package_main);

  if (!package_main.empty()) {
    const std::string initial =
        ubi_path::FromNamespacedPath(ubi_path::PathResolve({package_path, package_main}));
    for (int i = 0; i < 7; ++i) {
      if (internal_module_stat(initial + ext[i]) == 0) {
        napi_value out = nullptr;
        napi_create_int32(env, i, &out);
        return out != nullptr ? out : Undefined(env);
      }
    }
  }

  const std::string fallback =
      ubi_path::FromNamespacedPath(ubi_path::PathResolve({package_path, "./index"}));
  for (int i = 7; i < 10; ++i) {
    if (internal_module_stat(fallback + ext[i]) == 0) {
      napi_value out = nullptr;
      napi_create_int32(env, i, &out);
      return out != nullptr ? out : Undefined(env);
    }
  }

  napi_throw_error(env, "ERR_MODULE_NOT_FOUND", "Cannot find package main entry");
  return nullptr;
}

napi_value FsCpSyncCheckPaths(napi_env env, napi_callback_info info) {
  size_t argc = 4;
  napi_value argv[4] = {nullptr, nullptr, nullptr, nullptr};
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  std::string src;
  std::string dest;
  if (argc < 2 || !ValueToUtf8(env, argv[0], &src) || !ValueToUtf8(env, argv[1], &dest)) return Undefined(env);
  bool recursive = false;
  if (argc >= 4 && argv[3] != nullptr) napi_get_value_bool(env, argv[3], &recursive);

  auto normalize = [](std::string value) {
    std::replace(value.begin(), value.end(), '\\', '/');
    while (!value.empty() && value.back() == '/') value.pop_back();
    return value;
  };
  const std::string src_norm = normalize(src);
  const std::string dest_norm = normalize(dest);
  if (src_norm == dest_norm) {
    napi_throw_error(env, "EINVAL", "EINVAL: src and dest cannot be the same");
    return nullptr;
  }
  if (recursive && dest_norm.rfind(src_norm + "/", 0) == 0) {
    napi_throw_error(env, "EINVAL", "EINVAL: cannot copy to a subdirectory of itself");
    return nullptr;
  }
  return Undefined(env);
}

napi_value FsCpSyncOverrideFile(napi_env env, napi_callback_info info) {
  size_t argc = 3;
  napi_value argv[3] = {nullptr, nullptr, nullptr};
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  std::string src;
  std::string dest;
  if (argc < 2 || !ValueToUtf8(env, argv[0], &src) || !ValueToUtf8(env, argv[1], &dest)) return Undefined(env);
  std::error_code ec;
  std::filesystem::remove(dest, ec);
  ec.clear();
  std::filesystem::copy_file(src, dest, std::filesystem::copy_options::overwrite_existing, ec);
  if (ec) {
    napi_throw_error(env, "ERR_SYSTEM_ERROR", ec.message().c_str());
    return nullptr;
  }
  return Undefined(env);
}

napi_value FsCpSyncCopyDir(napi_env env, napi_callback_info info) {
  size_t argc = 7;
  napi_value argv[7] = {nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr};
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  std::string src;
  std::string dest;
  if (argc < 2 || !ValueToUtf8(env, argv[0], &src) || !ValueToUtf8(env, argv[1], &dest)) return Undefined(env);
  bool force = false;
  bool error_on_exist = false;
  if (argc >= 3 && argv[2] != nullptr) napi_get_value_bool(env, argv[2], &force);
  if (argc >= 5 && argv[4] != nullptr) napi_get_value_bool(env, argv[4], &error_on_exist);

  std::error_code ec;
  std::filesystem::create_directories(dest, ec);
  if (ec) {
    napi_throw_error(env, "ERR_SYSTEM_ERROR", ec.message().c_str());
    return nullptr;
  }

  for (const auto& entry : std::filesystem::recursive_directory_iterator(src, ec)) {
    if (ec) break;
    const auto relative = std::filesystem::relative(entry.path(), src, ec);
    if (ec) break;
    const auto target = std::filesystem::path(dest) / relative;
    if (entry.is_directory()) {
      std::filesystem::create_directories(target, ec);
      if (ec) break;
      continue;
    }
    std::filesystem::create_directories(target.parent_path(), ec);
    if (ec) break;
    auto options = std::filesystem::copy_options::none;
    if (force && !error_on_exist) options = std::filesystem::copy_options::overwrite_existing;
    std::filesystem::copy_file(entry.path(), target, options, ec);
    if (ec && force) {
      ec.clear();
      std::filesystem::copy_file(entry.path(), target, std::filesystem::copy_options::overwrite_existing, ec);
    }
    if (ec) break;
  }
  if (ec) {
    napi_throw_error(env, "ERR_SYSTEM_ERROR", ec.message().c_str());
    return nullptr;
  }
  return Undefined(env);
}

napi_value FsLink(napi_env env, napi_callback_info info) {
  size_t argc = 3;
  napi_value argv[3] = {nullptr, nullptr, nullptr};
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  napi_value req = argc >= 3 ? argv[2] : nullptr;
  napi_value oncomplete = nullptr;
  ReqKind req_kind = ParseReq(env, req, &oncomplete);
  napi_value call_argv[2] = {argc >= 1 ? argv[0] : Undefined(env), argc >= 2 ? argv[1] : Undefined(env)};
  napi_value out = nullptr;
  napi_value err = nullptr;
  if (!CallRaw(env, "link", 2, call_argv, &out, &err)) {
    if (req_kind == ReqKind::kPromise) return MakeRejectedPromise(env, err);
    CompleteReq(env, req_kind, req, oncomplete, err, Undefined(env));
    if (req_kind == ReqKind::kCallback) return Undefined(env);
    if (err != nullptr) {
      napi_throw(env, err);
      return nullptr;
    }
    return Undefined(env);
  }
  if (req_kind == ReqKind::kPromise) return MakeResolvedPromise(env, Undefined(env));
  CompleteReq(env, req_kind, req, oncomplete, nullptr, Undefined(env));
  return Undefined(env);
}

napi_value FsFdatasync(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value argv[2] = {nullptr, nullptr};
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  int32_t fd = -1;
  if (!ValidateFdArg(env, argc >= 1 ? argv[0] : nullptr, &fd)) return nullptr;
  napi_value req = argc >= 2 ? argv[1] : nullptr;
  napi_value oncomplete = nullptr;
  ReqKind req_kind = ParseReq(env, req, &oncomplete);
  napi_value call_argv[1] = {argc >= 1 ? argv[0] : Undefined(env)};
  napi_value out = nullptr;
  napi_value err = nullptr;
  if (!CallRaw(env, "fdatasync", 1, call_argv, &out, &err)) {
    if (!CallRaw(env, "fsync", 1, call_argv, &out, &err)) {
      if (req_kind == ReqKind::kPromise) return MakeRejectedPromise(env, err);
      CompleteReq(env, req_kind, req, oncomplete, err, Undefined(env));
      if (req_kind == ReqKind::kCallback) return Undefined(env);
      if (err != nullptr) {
        napi_throw(env, err);
        return nullptr;
      }
      return Undefined(env);
    }
  }
  if (req_kind == ReqKind::kPromise) return MakeResolvedPromise(env, Undefined(env));
  CompleteReq(env, req_kind, req, oncomplete, nullptr, Undefined(env));
  return Undefined(env);
}

napi_value FsChownCommon(napi_env env, napi_callback_info info, const char* method_name) {
  size_t argc = 4;
  napi_value argv[4] = {nullptr, nullptr, nullptr, nullptr};
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  napi_value req = argc >= 4 ? argv[3] : nullptr;
  napi_value oncomplete = nullptr;
  ReqKind req_kind = ParseReq(env, req, &oncomplete);
  napi_value call_argv[3] = {argc >= 1 ? argv[0] : Undefined(env),
                             argc >= 2 ? argv[1] : Undefined(env),
                             argc >= 3 ? argv[2] : Undefined(env)};
  return CompleteVoidRawFsMethod(env, method_name, req_kind, req, oncomplete, 3, call_argv);
}

napi_value FsChown(napi_env env, napi_callback_info info) {
  return FsChownCommon(env, info, "chown");
}

napi_value FsFchown(napi_env env, napi_callback_info info) {
  size_t argc = 4;
  napi_value argv[4] = {nullptr, nullptr, nullptr, nullptr};
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  int32_t fd = -1;
  if (!ValidateFdArg(env, argc >= 1 ? argv[0] : nullptr, &fd)) return nullptr;
  return FsChownCommon(env, info, "fchown");
}

napi_value FsLchown(napi_env env, napi_callback_info info) {
  return FsChownCommon(env, info, "lchown");
}

void EnsureTypedArrayProperty(napi_env env,
                              napi_value binding,
                              const char* name,
                              napi_typedarray_type type,
                              size_t length) {
  bool has = false;
  if (napi_has_named_property(env, binding, name, &has) != napi_ok || has) return;
  napi_value ab = nullptr;
  void* data = nullptr;
  const size_t byte_length = (type == napi_bigint64_array ? sizeof(int64_t) : sizeof(double)) * length;
  if (napi_create_arraybuffer(env, byte_length, &data, &ab) != napi_ok || ab == nullptr || data == nullptr) return;
  std::memset(data, 0, byte_length);
  napi_value out = nullptr;
  if (napi_create_typedarray(env, type, length, ab, 0, &out) == napi_ok && out != nullptr) {
    napi_set_named_property(env, binding, name, out);
  }
}

void EnsureClassProperty(napi_env env,
                         napi_value binding,
                         const char* name,
                         napi_callback ctor,
                         const std::vector<napi_property_descriptor>& methods,
                         napi_ref* out_ref) {
  bool has = false;
  if (napi_has_named_property(env, binding, name, &has) == napi_ok && has) {
    if (out_ref != nullptr) {
      napi_value existing = nullptr;
      if (napi_get_named_property(env, binding, name, &existing) == napi_ok && existing != nullptr && *out_ref == nullptr) {
        napi_create_reference(env, existing, 1, out_ref);
      }
    }
    return;
  }
  napi_value cls = nullptr;
  if (napi_define_class(env,
                        name,
                        NAPI_AUTO_LENGTH,
                        ctor,
                        nullptr,
                        methods.size(),
                        methods.data(),
                        &cls) != napi_ok ||
      cls == nullptr) {
    return;
  }
  napi_set_named_property(env, binding, name, cls);
  if (out_ref != nullptr) napi_create_reference(env, cls, 1, out_ref);
}

}  // namespace

napi_value ResolveFs(napi_env env, const ResolveOptions& options) {
  if (options.callbacks.resolve_binding == nullptr) return Undefined(env);
  napi_value binding = options.callbacks.resolve_binding(env, options.state, "fs");
  if (binding == nullptr || IsUndefined(env, binding)) return Undefined(env);

  auto& state = g_fs_states[env];
  if (state.binding_ref == nullptr) {
    napi_create_reference(env, binding, 1, &state.binding_ref);
  }

  // Capture raw methods before overriding.
  const char* raw_names[] = {"open",       "close",     "access",   "accessSync", "readSync",  "writeSync",
                             "writeSyncString", "stat",  "lstat",    "fstat",      "statfs",    "mkdir",
                             "readdir",    "realpath",  "readlink", "rename",     "ftruncate", "rmdir",
                             "unlink",     "symlink",   "copyFile", "chmod",      "fchmod",    "utimes",
                             "futimes",    "lutimes",   "writeFileUtf8", "readFileUtf8", "mkdtemp", "fsync",
                             "fdatasync",  "link", "chown", "fchown", "lchown"};
  for (const char* name : raw_names) CaptureRawMethod(env, &state, binding, name);

  // Constants/symbols.
  if (state.k_use_promises_symbol_ref == nullptr && options.callbacks.resolve_binding != nullptr) {
    napi_value symbols_binding = options.callbacks.resolve_binding(env, options.state, "symbols");
    if (symbols_binding != nullptr && !IsUndefined(env, symbols_binding)) {
      napi_value candidate = nullptr;
      if (napi_get_named_property(env, symbols_binding, "fs_use_promises_symbol", &candidate) == napi_ok &&
          candidate != nullptr) {
        napi_create_reference(env, candidate, 1, &state.k_use_promises_symbol_ref);
      }
    }
  }
  bool has_k_use_promises = false;
  if (napi_has_named_property(env, binding, "kUsePromises", &has_k_use_promises) == napi_ok && !has_k_use_promises) {
    napi_value symbol = GetUsePromisesSymbol(env);
    if (symbol != nullptr) napi_set_named_property(env, binding, "kUsePromises", symbol);
  } else if (has_k_use_promises && state.k_use_promises_symbol_ref == nullptr) {
    napi_value symbol = nullptr;
    if (napi_get_named_property(env, binding, "kUsePromises", &symbol) == napi_ok && symbol != nullptr) {
      napi_create_reference(env, symbol, 1, &state.k_use_promises_symbol_ref);
    }
  }

  bool has_fields = false;
  if (napi_has_named_property(env, binding, "kFsStatsFieldsNumber", &has_fields) == napi_ok && !has_fields) {
    SetNamedInt(env, binding, "kFsStatsFieldsNumber", static_cast<int32_t>(kFsStatsLength));
  }
  EnsureTypedArrayProperty(env, binding, "statValues", napi_float64_array, 36);
  EnsureTypedArrayProperty(env, binding, "bigintStatValues", napi_bigint64_array, 36);
  EnsureTypedArrayProperty(env, binding, "statFsValues", napi_float64_array, kFsStatFsLength);
  EnsureTypedArrayProperty(env, binding, "bigintStatFsValues", napi_bigint64_array, kFsStatFsLength);

  EnsureClassProperty(env, binding, "FSReqCallback", FSReqCallbackCtor, {}, &state.fs_req_ctor_ref);

  EnsureClassProperty(env,
                      binding,
                      "StatWatcher",
                      StatWatcherCtor,
                      {
                          {"start", nullptr, StatWatcherStart, nullptr, nullptr, nullptr, napi_default, nullptr},
                          {"close", nullptr, StatWatcherClose, nullptr, nullptr, nullptr, napi_default, nullptr},
                          {"ref", nullptr, StatWatcherRef, nullptr, nullptr, nullptr, napi_default, nullptr},
                          {"unref", nullptr, StatWatcherUnref, nullptr, nullptr, nullptr, napi_default, nullptr},
                          {"getAsyncId", nullptr, StatWatcherGetAsyncId, nullptr, nullptr, nullptr, napi_default,
                           nullptr},
                      },
                      &state.stat_watcher_ctor_ref);

  EnsureClassProperty(env,
                      binding,
                      "FileHandle",
                      FileHandleCtor,
                      {
                          {"close", nullptr, FileHandleClose, nullptr, nullptr, nullptr, napi_default, nullptr},
                          {"releaseFD", nullptr, FileHandleReleaseFD, nullptr, nullptr, nullptr, napi_default, nullptr},
                          {"readStart", nullptr, FileHandleReadStart, nullptr, nullptr, nullptr, napi_default, nullptr},
                          {"readStop", nullptr, FileHandleReadStop, nullptr, nullptr, nullptr, napi_default, nullptr},
                          {"shutdown", nullptr, FileHandleShutdown, nullptr, nullptr, nullptr, napi_default, nullptr},
                          {"useUserBuffer", nullptr, FileHandleUseUserBuffer, nullptr, nullptr, nullptr, napi_default,
                           nullptr},
                          {"writev", nullptr, FileHandleWritev, nullptr, nullptr, nullptr, napi_default, nullptr},
                          {"writeBuffer", nullptr, FileHandleWriteBuffer, nullptr, nullptr, nullptr, napi_default,
                           nullptr},
                          {"writeAsciiString", nullptr, FileHandleWriteAsciiString, nullptr, nullptr, nullptr,
                           napi_default, nullptr},
                          {"writeUtf8String", nullptr, FileHandleWriteUtf8String, nullptr, nullptr, nullptr,
                           napi_default, nullptr},
                          {"writeUcs2String", nullptr, FileHandleWriteUcs2String, nullptr, nullptr, nullptr,
                           napi_default, nullptr},
                          {"writeLatin1String", nullptr, FileHandleWriteLatin1String, nullptr, nullptr, nullptr,
                           napi_default, nullptr},
                          {"getAsyncId", nullptr, FileHandleGetAsyncId, nullptr, nullptr, nullptr, napi_default,
                           nullptr},
                          {"isStreamBase", nullptr, nullptr, FileHandleIsStreamBase, nullptr, nullptr, napi_default,
                           nullptr},
                          {"fd", nullptr, nullptr, FileHandleGetFd, nullptr, nullptr, napi_default, nullptr},
                          {"_externalStream", nullptr, nullptr, FileHandleGetExternalStream, nullptr, nullptr,
                           napi_default, nullptr},
                          {"bytesRead", nullptr, nullptr, FileHandleGetBytesRead, nullptr, nullptr, napi_default,
                           nullptr},
                          {"bytesWritten", nullptr, nullptr, FileHandleGetBytesWritten, nullptr, nullptr,
                           napi_default, nullptr},
                          {"onread", nullptr, nullptr, FileHandleGetOnread, FileHandleSetOnread, nullptr,
                           napi_default, nullptr},
                      },
                      &state.file_handle_ctor_ref);

  // Missing API surface.
  SetNamedMethod(env, binding, "open", FsOpen);
  SetNamedMethod(env, binding, "close", FsClose);
  SetNamedMethod(env, binding, "access", FsAccess);
  SetNamedMethod(env, binding, "stat", FsStat);
  SetNamedMethod(env, binding, "lstat", FsLstat);
  SetNamedMethod(env, binding, "fstat", FsFstat);
  SetNamedMethod(env, binding, "statfs", FsStatfs);
  SetNamedMethod(env, binding, "readdir", FsReaddir);
  SetNamedMethod(env, binding, "realpath", FsRealpath);
  SetNamedMethod(env, binding, "read", FsRead);
  SetNamedMethod(env, binding, "readBuffers", FsReadBuffers);
  SetNamedMethod(env, binding, "writeBuffer", FsWriteBuffer);
  SetNamedMethod(env, binding, "writeString", FsWriteString);
  SetNamedMethod(env, binding, "writeBuffers", FsWriteBuffers);
  SetNamedMethod(env, binding, "openFileHandle", FsOpenFileHandle);
  SetNamedMethod(env, binding, "internalModuleStat", FsInternalModuleStat);
  SetNamedMethod(env, binding, "legacyMainResolve", FsLegacyMainResolve);
  SetNamedMethod(env, binding, "cpSyncCheckPaths", FsCpSyncCheckPaths);
  SetNamedMethod(env, binding, "cpSyncOverrideFile", FsCpSyncOverrideFile);
  SetNamedMethod(env, binding, "cpSyncCopyDir", FsCpSyncCopyDir);
  SetNamedMethod(env, binding, "mkdtemp", FsMkdtemp);
  SetNamedMethod(env, binding, "mkdir", FsMkdir);
  SetNamedMethod(env, binding, "rename", FsRename);
  SetNamedMethod(env, binding, "copyFile", FsCopyFile);
  SetNamedMethod(env, binding, "readlink", FsReadlink);
  SetNamedMethod(env, binding, "symlink", FsSymlink);
  SetNamedMethod(env, binding, "unlink", FsUnlink);
  SetNamedMethod(env, binding, "rmdir", FsRmdir);
  SetNamedMethod(env, binding, "ftruncate", FsFtruncate);
  SetNamedMethod(env, binding, "fsync", FsFsync);
  SetNamedMethod(env, binding, "chmod", FsChmod);
  SetNamedMethod(env, binding, "fchmod", FsFchmod);
  SetNamedMethod(env, binding, "utimes", FsUtimes);
  SetNamedMethod(env, binding, "futimes", FsFutimes);
  SetNamedMethod(env, binding, "lutimes", FsLutimes);
  SetNamedMethod(env, binding, "link", FsLink);
  SetNamedMethod(env, binding, "fdatasync", FsFdatasync);
  SetNamedMethod(env, binding, "chown", FsChown);
  SetNamedMethod(env, binding, "fchown", FsFchown);
  SetNamedMethod(env, binding, "lchown", FsLchown);

  const char* hidden_props[] = {
      "accessSync",        "readSync",         "writeSync",         "writeSyncString",
      "O_RDONLY",          "O_WRONLY",         "O_RDWR",            "O_CREAT",
      "O_TRUNC",           "O_APPEND",         "O_EXCL",            "O_SYNC",
      "O_NOATIME",         "UV_DIRENT_UNKNOWN","UV_DIRENT_FILE",    "UV_DIRENT_DIR",
      "UV_DIRENT_LINK",    "UV_DIRENT_FIFO",   "UV_DIRENT_SOCKET",  "UV_DIRENT_CHAR",
      "UV_DIRENT_BLOCK",   "F_OK",             "R_OK",              "W_OK",
      "X_OK",              "S_IFMT",           "S_IFREG",           "S_IFDIR",
      "S_IFBLK",           "S_IFCHR",          "S_IFLNK",           "S_IFIFO",
      "S_IFSOCK",          "S_IRWXU",          "S_IRUSR",           "S_IWUSR",
      "S_IXUSR",           "S_IRWXG",          "S_IRGRP",           "S_IWGRP",
      "S_IXGRP",           "S_IRWXO",          "S_IROTH",           "S_IWOTH",
      "S_IXOTH",           "COPYFILE_EXCL",    "COPYFILE_FICLONE",  "COPYFILE_FICLONE_FORCE",
      "UV_FS_COPYFILE_EXCL","UV_FS_COPYFILE_FICLONE","UV_FS_COPYFILE_FICLONE_FORCE",
      "UV_FS_SYMLINK_DIR", "UV_FS_SYMLINK_JUNCTION",
  };
  for (const char* name : hidden_props) DeleteNamedProperty(env, binding, name);

  return binding;
}

}  // namespace internal_binding
