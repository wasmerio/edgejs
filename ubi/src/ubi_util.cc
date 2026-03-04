#include "ubi_util.h"

#include "unofficial_napi.h"

#include <uv.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

constexpr int32_t kPromisePending = 0;
constexpr int32_t kPromiseFulfilled = 1;
constexpr int32_t kPromiseRejected = 2;

constexpr int32_t kExitInfoKExiting = 0;
constexpr int32_t kExitInfoKExitCode = 1;
constexpr int32_t kExitInfoKHasExitCode = 2;

constexpr uint32_t kMaxArrayIndex = 4294967294u;

struct LazyPropertyData {
  std::string module_id;
  std::string key;
  bool enumerable = true;
};

std::vector<std::unique_ptr<LazyPropertyData>> g_lazy_property_data;
std::unordered_map<napi_env, napi_ref> g_types_binding_refs;

napi_value GetRefValue(napi_env env, napi_ref ref) {
  if (ref == nullptr) return nullptr;
  napi_value value = nullptr;
  if (napi_get_reference_value(env, ref, &value) != napi_ok || value == nullptr) return nullptr;
  return value;
}

napi_value Undefined(napi_env env) {
  napi_value out = nullptr;
  napi_get_undefined(env, &out);
  return out;
}

napi_value Null(napi_env env) {
  napi_value out = nullptr;
  napi_get_null(env, &out);
  return out;
}

bool SetNamedProperty(napi_env env, napi_value target, const char* key, napi_value value) {
  return target != nullptr && value != nullptr && napi_set_named_property(env, target, key, value) == napi_ok;
}

bool SetInt32(napi_env env, napi_value target, const char* key, int32_t value) {
  napi_value v = nullptr;
  return napi_create_int32(env, value, &v) == napi_ok && v != nullptr && SetNamedProperty(env, target, key, v);
}

bool SetBool(napi_env env, napi_value target, const char* key, bool value) {
  napi_value v = nullptr;
  return napi_get_boolean(env, value, &v) == napi_ok && v != nullptr && SetNamedProperty(env, target, key, v);
}

bool SetString(napi_env env, napi_value target, const char* key, const char* value) {
  napi_value v = nullptr;
  return napi_create_string_utf8(env, value, NAPI_AUTO_LENGTH, &v) == napi_ok && v != nullptr &&
         SetNamedProperty(env, target, key, v);
}

napi_value GetNamedProperty(napi_env env, napi_value obj, const char* key) {
  if (obj == nullptr) return nullptr;
  bool has_prop = false;
  if (napi_has_named_property(env, obj, key, &has_prop) != napi_ok || !has_prop) return nullptr;
  napi_value out = nullptr;
  if (napi_get_named_property(env, obj, key, &out) != napi_ok || out == nullptr) return nullptr;
  return out;
}

napi_value GetGlobal(napi_env env) {
  napi_value global = nullptr;
  if (napi_get_global(env, &global) != napi_ok || global == nullptr) return nullptr;
  return global;
}

napi_value GetGlobalNamed(napi_env env, const char* key) {
  return GetNamedProperty(env, GetGlobal(env), key);
}

bool IsFunction(napi_env env, napi_value value) {
  napi_valuetype t = napi_undefined;
  return value != nullptr && napi_typeof(env, value, &t) == napi_ok && t == napi_function;
}

bool IsObjectLike(napi_env env, napi_value value) {
  napi_valuetype t = napi_undefined;
  if (value == nullptr || napi_typeof(env, value, &t) != napi_ok) return false;
  return t == napi_object || t == napi_function;
}

std::string ToUtf8(napi_env env, napi_value value) {
  if (value == nullptr) return "";
  napi_value str = nullptr;
  if (napi_coerce_to_string(env, value, &str) != napi_ok || str == nullptr) return "";
  size_t length = 0;
  if (napi_get_value_string_utf8(env, str, nullptr, 0, &length) != napi_ok) return "";
  std::string out(length + 1, '\0');
  size_t copied = 0;
  if (napi_get_value_string_utf8(env, str, out.data(), out.size(), &copied) != napi_ok) return "";
  out.resize(copied);
  return out;
}

bool ValueToUtf8IfString(napi_env env, napi_value value, std::string* out) {
  if (value == nullptr || out == nullptr) return false;
  napi_valuetype t = napi_undefined;
  if (napi_typeof(env, value, &t) != napi_ok || t != napi_string) return false;
  *out = ToUtf8(env, value);
  return true;
}

napi_value CallFunction(napi_env env,
                        napi_value this_arg,
                        napi_value fn,
                        size_t argc,
                        napi_value* argv) {
  if (this_arg == nullptr) this_arg = Undefined(env);
  if (!IsFunction(env, fn)) return nullptr;
  napi_value out = nullptr;
  if (napi_call_function(env, this_arg, fn, argc, argv, &out) != napi_ok) return nullptr;
  return out;
}

napi_value NewInstance(napi_env env, napi_value ctor, size_t argc, napi_value* argv) {
  if (!IsFunction(env, ctor)) return nullptr;
  napi_value out = nullptr;
  if (napi_new_instance(env, ctor, argc, argv, &out) != napi_ok) return nullptr;
  return out;
}

napi_value CreateMap(napi_env env) {
  napi_value map_ctor = GetGlobalNamed(env, "Map");
  if (!IsFunction(env, map_ctor)) return nullptr;
  return NewInstance(env, map_ctor, 0, nullptr);
}

bool MapSet(napi_env env, napi_value map, napi_value key, napi_value value) {
  napi_value set_fn = GetNamedProperty(env, map, "set");
  if (!IsFunction(env, set_fn)) return false;
  napi_value argv[2] = {key, value};
  return CallFunction(env, map, set_fn, 2, argv) != nullptr;
}

bool IsArrayIndexString(std::string_view key) {
  if (key.empty()) return false;
  if (key.size() > 1 && key.front() == '0') return false;
  uint64_t value = 0;
  for (char ch : key) {
    if (!std::isdigit(static_cast<unsigned char>(ch))) return false;
    value = value * 10 + static_cast<unsigned>(ch - '0');
    if (value > kMaxArrayIndex) return false;
  }
  return true;
}

bool ValueToTagEquals(napi_env env, napi_value value, const char* expected) {
  napi_value object_ctor = GetGlobalNamed(env, "Object");
  napi_value object_prototype = GetNamedProperty(env, object_ctor, "prototype");
  napi_value to_string_fn = GetNamedProperty(env, object_prototype, "toString");
  if (!IsFunction(env, to_string_fn)) return false;
  napi_value out = CallFunction(env, value != nullptr ? value : Undefined(env), to_string_fn, 0, nullptr);
  if (out == nullptr) return false;
  return ToUtf8(env, out) == expected;
}

bool ValueInstanceOfGlobalCtor(napi_env env, napi_value value, const char* ctor_name) {
  if (!IsObjectLike(env, value)) return false;
  napi_value ctor = GetGlobalNamed(env, ctor_name);
  if (!IsFunction(env, ctor)) return false;
  bool result = false;
  if (napi_instanceof(env, value, ctor, &result) != napi_ok) return false;
  return result;
}

static uint32_t GetUVHandleTypeCode(uv_handle_type type) {
  switch (type) {
    case UV_TCP:
      return 0;
    case UV_TTY:
      return 1;
    case UV_UDP:
      return 2;
    case UV_FILE:
      return 3;
    case UV_NAMED_PIPE:
      return 4;
    case UV_UNKNOWN_HANDLE:
      return 5;
    default:
      return 5;
  }
}

napi_value GuessHandleType(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  if (napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr) != napi_ok || argc < 1) {
    return Undefined(env);
  }
  int32_t fd = -1;
  if (napi_get_value_int32(env, argv[0], &fd) != napi_ok || fd < 0) {
    return Undefined(env);
  }
  const uv_handle_type t = uv_guess_handle(static_cast<uv_file>(fd));
  napi_value result = nullptr;
  if (napi_create_uint32(env, GetUVHandleTypeCode(t), &result) != napi_ok || result == nullptr) {
    return Undefined(env);
  }
  return result;
}

napi_value IsInsideNodeModulesCallback(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);

  int32_t frame_limit = 10;
  if (argc >= 1 && argv[0] != nullptr) {
    napi_get_value_int32(env, argv[0], &frame_limit);
  }

  bool result = false;
  if (frame_limit > 1) {
    uint32_t frames = static_cast<uint32_t>(frame_limit - 1);
    if (frames > 200) frames = 200;

    napi_value callsites = nullptr;
    if (unofficial_napi_get_call_sites(env, frames, &callsites) == napi_ok && callsites != nullptr) {
      bool is_array = false;
      if (napi_is_array(env, callsites, &is_array) == napi_ok && is_array) {
        uint32_t length = 0;
        if (napi_get_array_length(env, callsites, &length) == napi_ok) {
          for (uint32_t i = 0; i < length; ++i) {
            napi_value callsite = nullptr;
            if (napi_get_element(env, callsites, i, &callsite) != napi_ok || callsite == nullptr) continue;

            napi_value script_name = GetNamedProperty(env, callsite, "scriptName");
            std::string script_name_str;
            if (!ValueToUtf8IfString(env, script_name, &script_name_str)) continue;
            if (script_name_str.empty()) continue;
            if (script_name_str.rfind("node:", 0) == 0) continue;

            result = script_name_str.find("/node_modules/") != std::string::npos ||
                     script_name_str.find("\\node_modules\\") != std::string::npos ||
                     script_name_str.find("/node_modules\\") != std::string::npos ||
                     script_name_str.find("\\node_modules/") != std::string::npos;
            break;
          }
        }
      }
    }
  }
  napi_value out = nullptr;
  napi_get_boolean(env, result, &out);
  return out != nullptr ? out : Undefined(env);
}

napi_value LazyPropertyGetter(napi_env env, napi_callback_info info) {
  size_t argc = 0;
  napi_value this_arg = nullptr;
  void* data = nullptr;
  if (napi_get_cb_info(env, info, &argc, nullptr, &this_arg, &data) != napi_ok || data == nullptr) {
    return Undefined(env);
  }

  auto* lazy = static_cast<LazyPropertyData*>(data);
  napi_value require_fn = GetGlobalNamed(env, "require");
  if (!IsFunction(env, require_fn)) return Undefined(env);

  napi_value id = nullptr;
  if (napi_create_string_utf8(env, lazy->module_id.c_str(), lazy->module_id.size(), &id) != napi_ok || id == nullptr) {
    return Undefined(env);
  }

  napi_value module = nullptr;
  napi_value argv_require[1] = {id};
  module = CallFunction(env, GetGlobal(env), require_fn, 1, argv_require);
  if (!IsObjectLike(env, module)) return Undefined(env);

  napi_value value = nullptr;
  if (napi_get_named_property(env, module, lazy->key.c_str(), &value) != napi_ok || value == nullptr) {
    value = Undefined(env);
  }

  if (IsObjectLike(env, this_arg)) {
    napi_property_descriptor descriptor = {
        .utf8name = lazy->key.c_str(),
        .name = nullptr,
        .method = nullptr,
        .getter = nullptr,
        .setter = nullptr,
        .value = value,
        .attributes = static_cast<napi_property_attributes>(napi_writable | napi_configurable |
                                                            (lazy->enumerable ? napi_enumerable : napi_default)),
        .data = nullptr,
    };
    napi_define_properties(env, this_arg, 1, &descriptor);
  }

  return value;
}

napi_value DefineLazyPropertiesCallback(napi_env env, napi_callback_info info) {
  size_t argc = 4;
  napi_value argv[4] = {nullptr, nullptr, nullptr, nullptr};
  if (napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr) != napi_ok || argc < 3) {
    return Undefined(env);
  }

  napi_value target = argv[0];
  napi_value id = argv[1];
  napi_value keys = argv[2];

  bool is_array = false;
  napi_is_array(env, keys, &is_array);
  if (!IsObjectLike(env, target) || !is_array) {
    return Undefined(env);
  }

  bool enumerable = true;
  if (argc >= 4 && argv[3] != nullptr) {
    napi_get_value_bool(env, argv[3], &enumerable);
  }

  const std::string module_id = ToUtf8(env, id);

  uint32_t key_count = 0;
  if (napi_get_array_length(env, keys, &key_count) != napi_ok) return Undefined(env);
  for (uint32_t i = 0; i < key_count; i++) {
    napi_value key_val = nullptr;
    if (napi_get_element(env, keys, i, &key_val) != napi_ok || key_val == nullptr) continue;
    const std::string key = ToUtf8(env, key_val);
    if (key.empty()) continue;

    auto data = std::make_unique<LazyPropertyData>();
    data->module_id = module_id;
    data->key = key;
    data->enumerable = enumerable;
    LazyPropertyData* raw_data = data.get();
    g_lazy_property_data.emplace_back(std::move(data));

    napi_property_descriptor descriptor = {
        .utf8name = raw_data->key.c_str(),
        .name = nullptr,
        .method = nullptr,
        .getter = LazyPropertyGetter,
        .setter = nullptr,
        .value = nullptr,
        .attributes = static_cast<napi_property_attributes>(napi_configurable |
                                                            (enumerable ? napi_enumerable : napi_default)),
        .data = raw_data,
    };
    napi_define_properties(env, target, 1, &descriptor);
  }

  return Undefined(env);
}

napi_value ConstructSharedArrayBufferCallback(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  if (napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr) != napi_ok || argc < 1) {
    return Undefined(env);
  }

  napi_value num = nullptr;
  if (napi_coerce_to_number(env, argv[0], &num) != napi_ok || num == nullptr) {
    return Undefined(env);
  }

  int64_t length = 0;
  if (napi_get_value_int64(env, num, &length) != napi_ok) {
    return Undefined(env);
  }
  if (length < 0) {
    napi_throw_range_error(env, nullptr, "Invalid array buffer length");
    return nullptr;
  }

  napi_value ctor = GetGlobalNamed(env, "SharedArrayBuffer");
  if (!IsFunction(env, ctor)) {
    napi_throw_error(env, nullptr, "SharedArrayBuffer is not available");
    return nullptr;
  }

  napi_value len_arg = nullptr;
  if (napi_create_int64(env, length, &len_arg) != napi_ok || len_arg == nullptr) {
    return Undefined(env);
  }

  napi_value argv_ctor[1] = {len_arg};
  napi_value out = NewInstance(env, ctor, 1, argv_ctor);
  if (out == nullptr) {
    napi_throw_range_error(env, nullptr, "Array buffer allocation failed");
    return nullptr;
  }
  return out;
}

napi_value GetOwnNonIndexPropertiesCallback(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value argv[2] = {nullptr, nullptr};
  if (napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr) != napi_ok || argc < 2) {
    return Undefined(env);
  }

  napi_value source = argv[0];
  uint32_t filter_bits = 0;
  napi_get_value_uint32(env, argv[1], &filter_bits);

  napi_value keys = nullptr;
  if (napi_get_all_property_names(env,
                                  source,
                                  napi_key_own_only,
                                  static_cast<napi_key_filter>(filter_bits),
                                  napi_key_numbers_to_strings,
                                  &keys) != napi_ok ||
      keys == nullptr) {
    return Undefined(env);
  }

  uint32_t key_count = 0;
  napi_get_array_length(env, keys, &key_count);

  napi_value out = nullptr;
  if (napi_create_array(env, &out) != napi_ok || out == nullptr) return Undefined(env);

  uint32_t out_idx = 0;
  for (uint32_t i = 0; i < key_count; i++) {
    napi_value key = nullptr;
    if (napi_get_element(env, keys, i, &key) != napi_ok || key == nullptr) continue;

    napi_valuetype type = napi_undefined;
    if (napi_typeof(env, key, &type) != napi_ok) continue;
    if (type != napi_string) {
      napi_set_element(env, out, out_idx++, key);
      continue;
    }

    const std::string text = ToUtf8(env, key);
    if (IsArrayIndexString(text)) continue;
    napi_set_element(env, out, out_idx++, key);
  }

  return out;
}

napi_value GetConstructorNameCallback(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  if (argc < 1 || argv[0] == nullptr) {
    napi_value empty = nullptr;
    napi_create_string_utf8(env, "", NAPI_AUTO_LENGTH, &empty);
    return empty != nullptr ? empty : Undefined(env);
  }

  napi_value out = nullptr;
  if (unofficial_napi_get_constructor_name(env, argv[0], &out) == napi_ok && out != nullptr) {
    return out;
  }

  napi_value empty = nullptr;
  napi_create_string_utf8(env, "", NAPI_AUTO_LENGTH, &empty);
  return empty != nullptr ? empty : Undefined(env);
}

napi_value GetExternalValueCallback(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);

  uint64_t value = 0;
  if (argc >= 1 && argv[0] != nullptr) {
    napi_valuetype t = napi_undefined;
    if (napi_typeof(env, argv[0], &t) == napi_ok && t == napi_external) {
      void* ptr = nullptr;
      if (napi_get_value_external(env, argv[0], &ptr) == napi_ok) {
        value = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(ptr));
      }
    }
  }

  napi_value out = nullptr;
  if (napi_create_bigint_uint64(env, value, &out) != napi_ok || out == nullptr) return Undefined(env);
  return out;
}

napi_value GetPromiseDetailsCallback(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  if (argc < 1 || argv[0] == nullptr) return Undefined(env);

  bool is_promise = false;
  if (napi_is_promise(env, argv[0], &is_promise) != napi_ok || !is_promise) return Undefined(env);

  int32_t state = 0;
  bool has_result = false;
  napi_value result = nullptr;
  if (unofficial_napi_get_promise_details(env, argv[0], &state, &result, &has_result) != napi_ok) {
    return Undefined(env);
  }

  napi_value out = nullptr;
  if (napi_create_array_with_length(env, has_result ? 2 : 1, &out) != napi_ok || out == nullptr) {
    return Undefined(env);
  }
  napi_value state_v = nullptr;
  napi_create_int32(env, state, &state_v);
  napi_set_element(env, out, 0, state_v);
  if (has_result && result != nullptr) {
    napi_set_element(env, out, 1, result);
  }
  return out;
}

napi_value GetProxyDetailsCallback(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value argv[2] = {nullptr, nullptr};
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  if (argc < 1 || argv[0] == nullptr) return Undefined(env);

  napi_value target = nullptr;
  napi_value handler = nullptr;
  if (unofficial_napi_get_proxy_details(env, argv[0], &target, &handler) != napi_ok || target == nullptr ||
      handler == nullptr) {
    return Undefined(env);
  }

  bool full = true;
  if (argc >= 2 && argv[1] != nullptr) {
    napi_get_value_bool(env, argv[1], &full);
  }

  if (!full) return target;

  napi_value out = nullptr;
  if (napi_create_array_with_length(env, 2, &out) != napi_ok || out == nullptr) return Undefined(env);
  napi_set_element(env, out, 0, target);
  napi_set_element(env, out, 1, handler);
  return out;
}

napi_value GetCallerLocationCallback(napi_env env, napi_callback_info /*info*/) {
  napi_value callsites = nullptr;
  if (unofficial_napi_get_call_sites(env, 2, &callsites) != napi_ok || callsites == nullptr) {
    return Undefined(env);
  }

  bool is_array = false;
  if (napi_is_array(env, callsites, &is_array) != napi_ok || !is_array) return Undefined(env);

  uint32_t length = 0;
  if (napi_get_array_length(env, callsites, &length) != napi_ok || length < 1) return Undefined(env);

  napi_value callsite = nullptr;
  if (napi_get_element(env, callsites, 0, &callsite) != napi_ok || callsite == nullptr) return Undefined(env);

  napi_value file = GetNamedProperty(env, callsite, "scriptNameOrSourceURL");
  std::string file_str;
  ValueToUtf8IfString(env, file, &file_str);
  if (file_str.empty()) {
    file = GetNamedProperty(env, callsite, "scriptName");
    file_str.clear();
    ValueToUtf8IfString(env, file, &file_str);
  }
  if (file_str.empty()) {
    if (napi_create_string_utf8(env, "[eval]", NAPI_AUTO_LENGTH, &file) != napi_ok || file == nullptr) {
      return Undefined(env);
    }
  }

  napi_value line_v = GetNamedProperty(env, callsite, "lineNumber");
  napi_value column_v = GetNamedProperty(env, callsite, "columnNumber");
  if (line_v == nullptr || column_v == nullptr) return Undefined(env);

  int32_t line = 0;
  int32_t column = 0;
  if (napi_get_value_int32(env, line_v, &line) != napi_ok || napi_get_value_int32(env, column_v, &column) != napi_ok) {
    return Undefined(env);
  }

  napi_value out = nullptr;
  if (napi_create_array_with_length(env, 3, &out) != napi_ok || out == nullptr) return Undefined(env);

  napi_value line_out = nullptr;
  napi_value column_out = nullptr;
  if (napi_create_int32(env, line, &line_out) != napi_ok || line_out == nullptr) return Undefined(env);
  if (napi_create_int32(env, column, &column_out) != napi_ok || column_out == nullptr) return Undefined(env);
  if (napi_set_element(env, out, 0, line_out) != napi_ok || napi_set_element(env, out, 1, column_out) != napi_ok ||
      napi_set_element(env, out, 2, file) != napi_ok) {
    return Undefined(env);
  }
  return out;
}

napi_value PreviewEntriesCallback(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value argv[2] = {nullptr, nullptr};
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  if (argc < 1 || argv[0] == nullptr) return Undefined(env);

  napi_value entries = nullptr;
  bool is_key_value = false;
  if (unofficial_napi_preview_entries(env, argv[0], &entries, &is_key_value) != napi_ok || entries == nullptr) {
    return Undefined(env);
  }

  if (argc < 2) return entries;

  napi_value out = nullptr;
  if (napi_create_array_with_length(env, 2, &out) != napi_ok || out == nullptr) return Undefined(env);
  napi_set_element(env, out, 0, entries);
  napi_value is_key_value_v = nullptr;
  napi_get_boolean(env, is_key_value, &is_key_value_v);
  napi_set_element(env, out, 1, is_key_value_v);
  return out;
}

napi_value SleepCallback(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  if (argc < 1 || argv[0] == nullptr) return Undefined(env);

  uint32_t msec = 0;
  if (napi_get_value_uint32(env, argv[0], &msec) != napi_ok) return Undefined(env);
  uv_sleep(msec);
  return Undefined(env);
}

std::string_view TrimSpaces(std::string_view input) {
  if (input.empty()) return "";
  size_t start = input.find_first_not_of(" \t\n");
  if (start == std::string_view::npos) return "";
  size_t end = input.find_last_not_of(" \t\n");
  if (end == std::string_view::npos) return input.substr(start);
  return input.substr(start, end - start + 1);
}

std::map<std::string, std::string> ParseDotenvContent(const std::string& input) {
  std::map<std::string, std::string> store;

  std::string lines = input;
  lines.erase(std::remove(lines.begin(), lines.end(), '\r'), lines.end());
  std::string_view content = TrimSpaces(lines);

  while (!content.empty()) {
    if (content.front() == '\n' || content.front() == '#') {
      size_t newline = content.find('\n');
      if (newline == std::string_view::npos) {
        content = {};
      } else {
        content.remove_prefix(newline + 1);
      }
      continue;
    }

    size_t equal_or_newline = content.find_first_of("=\n");
    if (equal_or_newline == std::string_view::npos || content[equal_or_newline] == '\n') {
      if (equal_or_newline == std::string_view::npos) break;
      content.remove_prefix(equal_or_newline + 1);
      content = TrimSpaces(content);
      continue;
    }

    std::string_view key = TrimSpaces(content.substr(0, equal_or_newline));
    content.remove_prefix(equal_or_newline + 1);

    if (key.starts_with("export ")) {
      key.remove_prefix(7);
      key = TrimSpaces(key);
    }

    if (key.empty()) {
      size_t newline = content.find('\n');
      if (newline == std::string_view::npos) break;
      content.remove_prefix(newline + 1);
      content = TrimSpaces(content);
      continue;
    }

    if (content.empty() || content.front() == '\n') {
      store[std::string(key)] = "";
      if (!content.empty()) content.remove_prefix(1);
      continue;
    }

    content = TrimSpaces(content);
    if (content.empty()) {
      store[std::string(key)] = "";
      break;
    }

    if (content.front() == '"') {
      size_t closing = content.find('"', 1);
      if (closing != std::string_view::npos) {
        std::string value(content.substr(1, closing - 1));
        size_t pos = 0;
        while ((pos = value.find("\\n", pos)) != std::string::npos) {
          value.replace(pos, 2, "\n");
          pos += 1;
        }
        store[std::string(key)] = value;
        size_t newline = content.find('\n', closing + 1);
        if (newline == std::string_view::npos) {
          content = {};
        } else {
          content.remove_prefix(newline + 1);
        }
        content = TrimSpaces(content);
        continue;
      }
    }

    if (content.front() == '\'' || content.front() == '"' || content.front() == '`') {
      char quote = content.front();
      size_t closing = content.find(quote, 1);
      if (closing == std::string_view::npos) {
        size_t newline = content.find('\n');
        std::string value(newline == std::string_view::npos ? content : content.substr(0, newline));
        store[std::string(key)] = value;
        if (newline == std::string_view::npos) {
          content = {};
        } else {
          content.remove_prefix(newline + 1);
        }
      } else {
        store[std::string(key)] = std::string(content.substr(1, closing - 1));
        size_t newline = content.find('\n', closing + 1);
        if (newline == std::string_view::npos) {
          content = {};
        } else {
          content.remove_prefix(newline + 1);
        }
      }
      content = TrimSpaces(content);
      continue;
    }

    size_t newline = content.find('\n');
    std::string_view value = (newline == std::string_view::npos) ? content : content.substr(0, newline);
    size_t hash = value.find('#');
    if (hash != std::string_view::npos) value = value.substr(0, hash);
    store[std::string(key)] = std::string(TrimSpaces(value));
    if (newline == std::string_view::npos) {
      content = {};
    } else {
      content.remove_prefix(newline + 1);
      content = TrimSpaces(content);
    }
  }

  return store;
}

napi_value ParseEnvCallback(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  if (napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr) != napi_ok || argc < 1 || argv[0] == nullptr) {
    return Undefined(env);
  }

  const std::string content = ToUtf8(env, argv[0]);
  const std::map<std::string, std::string> parsed = ParseDotenvContent(content);

  napi_value out = nullptr;
  if (napi_create_object(env, &out) != napi_ok || out == nullptr) return Undefined(env);

  for (const auto& [key, value] : parsed) {
    napi_value value_v = nullptr;
    if (napi_create_string_utf8(env, value.c_str(), value.size(), &value_v) == napi_ok && value_v != nullptr) {
      napi_set_named_property(env, out, key.c_str(), value_v);
    }
  }

  return out;
}

napi_value ArrayBufferViewHasBufferCallback(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);

  bool result = false;
  if (argc >= 1 && argv[0] != nullptr) {
    unofficial_napi_arraybuffer_view_has_buffer(env, argv[0], &result);
  }
  napi_value out = nullptr;
  napi_get_boolean(env, result, &out);
  return out != nullptr ? out : Undefined(env);
}

napi_value GetCallSitesCallback(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  if (argc < 1 || argv[0] == nullptr) return Undefined(env);

  uint32_t frames = 0;
  if (napi_get_value_uint32(env, argv[0], &frames) != napi_ok) return Undefined(env);

  napi_value out = nullptr;
  if (unofficial_napi_get_call_sites(env, frames, &out) != napi_ok || out == nullptr) return Undefined(env);
  return out;
}

bool DefineMethod(napi_env env, napi_value target, const char* name, napi_callback cb, void* data = nullptr) {
  napi_value fn = nullptr;
  if (napi_create_function(env, name, NAPI_AUTO_LENGTH, cb, data, &fn) != napi_ok || fn == nullptr) return false;
  return SetNamedProperty(env, target, name, fn);
}

napi_value CreateSymbol(napi_env env, const char* description) {
  napi_value desc = nullptr;
  if (napi_create_string_utf8(env, description, NAPI_AUTO_LENGTH, &desc) != napi_ok || desc == nullptr) {
    return nullptr;
  }
  napi_value sym = nullptr;
  if (napi_create_symbol(env, desc, &sym) != napi_ok || sym == nullptr) return nullptr;
  return sym;
}

bool InstallPrivateSymbols(napi_env env, napi_value binding) {
  napi_value private_symbols = nullptr;
  if (napi_create_object(env, &private_symbols) != napi_ok || private_symbols == nullptr) return false;

  const std::array<const char*, 16> symbol_names = {
      "untransferable_object_private_symbol",
      "arrow_message_private_symbol",
      "decorated_private_symbol",
      "exit_info_private_symbol",
      "contextify_context_private_symbol",
      "host_defined_option_symbol",
      "entry_point_promise_private_symbol",
      "entry_point_module_private_symbol",
      "module_source_private_symbol",
      "module_export_names_private_symbol",
      "module_circular_visited_private_symbol",
      "module_export_private_symbol",
      "module_first_parent_private_symbol",
      "module_last_parent_private_symbol",
      "transfer_mode_private_symbol",
      "source_map_data_private_symbol",
  };

  for (const char* key : symbol_names) {
    napi_value sym = CreateSymbol(env, key);
    if (sym == nullptr) return false;
    if (!SetNamedProperty(env, private_symbols, key, sym)) return false;
  }

  return SetNamedProperty(env, binding, "privateSymbols", private_symbols);
}

bool InstallConstants(napi_env env, napi_value binding) {
  napi_value constants = nullptr;
  if (napi_create_object(env, &constants) != napi_ok || constants == nullptr) return false;

  if (!SetInt32(env, constants, "kPending", kPromisePending) ||
      !SetInt32(env, constants, "kFulfilled", kPromiseFulfilled) ||
      !SetInt32(env, constants, "kRejected", kPromiseRejected) ||
      !SetInt32(env, constants, "kExiting", kExitInfoKExiting) ||
      !SetInt32(env, constants, "kExitCode", kExitInfoKExitCode) ||
      !SetInt32(env, constants, "kHasExitCode", kExitInfoKHasExitCode) ||
      !SetInt32(env, constants, "ALL_PROPERTIES", 0) ||
      !SetInt32(env, constants, "ONLY_WRITABLE", 1) ||
      !SetInt32(env, constants, "ONLY_ENUMERABLE", 2) ||
      !SetInt32(env, constants, "ONLY_CONFIGURABLE", 4) ||
      !SetInt32(env, constants, "SKIP_STRINGS", 8) ||
      !SetInt32(env, constants, "SKIP_SYMBOLS", 16) ||
      !SetInt32(env, constants, "kDisallowCloneAndTransfer", 0) ||
      !SetInt32(env, constants, "kTransferable", 1) ||
      !SetInt32(env, constants, "kCloneable", 2)) {
    return false;
  }

  return SetNamedProperty(env, binding, "constants", constants);
}

bool InstallShouldAbortToggle(napi_env env, napi_value binding) {
  napi_value ab = nullptr;
  void* data = nullptr;
  if (napi_create_arraybuffer(env, 1, &data, &ab) != napi_ok || ab == nullptr || data == nullptr) return false;
  static_cast<uint8_t*>(data)[0] = 1;

  napi_value out = nullptr;
  if (napi_create_typedarray(env, napi_uint8_array, 1, ab, 0, &out) != napi_ok || out == nullptr) return false;
  return SetNamedProperty(env, binding, "shouldAbortOnUncaughtToggle", out);
}

enum class TypeCheckKind : uintptr_t {
  kExternal,
  kDate,
  kArgumentsObject,
  kBooleanObject,
  kNumberObject,
  kStringObject,
  kSymbolObject,
  kBigIntObject,
  kNativeError,
  kRegExp,
  kAsyncFunction,
  kGeneratorFunction,
  kGeneratorObject,
  kPromise,
  kMap,
  kSet,
  kMapIterator,
  kSetIterator,
  kWeakMap,
  kWeakSet,
  kArrayBuffer,
  kDataView,
  kSharedArrayBuffer,
  kProxy,
  kModuleNamespaceObject,
  kAnyArrayBuffer,
  kBoxedPrimitive,
};

bool RunTypeCheck(napi_env env, TypeCheckKind kind, napi_value value) {
  switch (kind) {
    case TypeCheckKind::kExternal: {
      napi_valuetype t = napi_undefined;
      return napi_typeof(env, value, &t) == napi_ok && t == napi_external;
    }
    case TypeCheckKind::kDate: {
      bool out = false;
      return napi_is_date(env, value, &out) == napi_ok && out;
    }
    case TypeCheckKind::kArgumentsObject:
      return ValueToTagEquals(env, value, "[object Arguments]");
    case TypeCheckKind::kBooleanObject:
      return ValueToTagEquals(env, value, "[object Boolean]");
    case TypeCheckKind::kNumberObject:
      return ValueToTagEquals(env, value, "[object Number]");
    case TypeCheckKind::kStringObject:
      return ValueToTagEquals(env, value, "[object String]");
    case TypeCheckKind::kSymbolObject:
      return ValueToTagEquals(env, value, "[object Symbol]");
    case TypeCheckKind::kBigIntObject:
      return ValueToTagEquals(env, value, "[object BigInt]");
    case TypeCheckKind::kNativeError: {
      bool out = false;
      return napi_is_error(env, value, &out) == napi_ok && out;
    }
    case TypeCheckKind::kRegExp:
      return ValueToTagEquals(env, value, "[object RegExp]");
    case TypeCheckKind::kAsyncFunction:
      return ValueToTagEquals(env, value, "[object AsyncFunction]");
    case TypeCheckKind::kGeneratorFunction:
      return ValueToTagEquals(env, value, "[object GeneratorFunction]") ||
             ValueToTagEquals(env, value, "[object AsyncGeneratorFunction]");
    case TypeCheckKind::kGeneratorObject:
      return ValueToTagEquals(env, value, "[object Generator]");
    case TypeCheckKind::kPromise: {
      bool out = false;
      return napi_is_promise(env, value, &out) == napi_ok && out;
    }
    case TypeCheckKind::kMap:
      return ValueInstanceOfGlobalCtor(env, value, "Map");
    case TypeCheckKind::kSet:
      return ValueInstanceOfGlobalCtor(env, value, "Set");
    case TypeCheckKind::kMapIterator:
      return ValueToTagEquals(env, value, "[object Map Iterator]");
    case TypeCheckKind::kSetIterator:
      return ValueToTagEquals(env, value, "[object Set Iterator]");
    case TypeCheckKind::kWeakMap:
      return ValueInstanceOfGlobalCtor(env, value, "WeakMap");
    case TypeCheckKind::kWeakSet:
      return ValueInstanceOfGlobalCtor(env, value, "WeakSet");
    case TypeCheckKind::kArrayBuffer: {
      bool out = false;
      return napi_is_arraybuffer(env, value, &out) == napi_ok && out;
    }
    case TypeCheckKind::kDataView: {
      bool out = false;
      return napi_is_dataview(env, value, &out) == napi_ok && out;
    }
    case TypeCheckKind::kSharedArrayBuffer:
      return ValueToTagEquals(env, value, "[object SharedArrayBuffer]");
    case TypeCheckKind::kProxy: {
      napi_value target = nullptr;
      napi_value handler = nullptr;
      return unofficial_napi_get_proxy_details(env, value, &target, &handler) == napi_ok;
    }
    case TypeCheckKind::kModuleNamespaceObject:
      return ValueToTagEquals(env, value, "[object Module]");
    case TypeCheckKind::kAnyArrayBuffer:
      return RunTypeCheck(env, TypeCheckKind::kArrayBuffer, value) ||
             RunTypeCheck(env, TypeCheckKind::kSharedArrayBuffer, value);
    case TypeCheckKind::kBoxedPrimitive:
      return RunTypeCheck(env, TypeCheckKind::kNumberObject, value) ||
             RunTypeCheck(env, TypeCheckKind::kStringObject, value) ||
             RunTypeCheck(env, TypeCheckKind::kBooleanObject, value) ||
             RunTypeCheck(env, TypeCheckKind::kBigIntObject, value) ||
             RunTypeCheck(env, TypeCheckKind::kSymbolObject, value);
  }

  return false;
}

napi_value TypeCheckCallback(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  void* data = nullptr;
  napi_get_cb_info(env, info, &argc, argv, nullptr, &data);

  bool result = false;
  if (data != nullptr && argc >= 1 && argv[0] != nullptr) {
    result = RunTypeCheck(env, static_cast<TypeCheckKind>(reinterpret_cast<uintptr_t>(data)), argv[0]);
  }

  napi_value out = nullptr;
  napi_get_boolean(env, result, &out);
  return out != nullptr ? out : Undefined(env);
}

bool DefineTypePredicate(napi_env env, napi_value target, const char* name, TypeCheckKind kind) {
  return DefineMethod(env,
                      target,
                      name,
                      TypeCheckCallback,
                      reinterpret_cast<void*>(static_cast<uintptr_t>(kind)));
}

bool InstallTypesBinding(napi_env env) {
  napi_value types = nullptr;
  if (napi_create_object(env, &types) != napi_ok || types == nullptr) return false;

  if (!DefineTypePredicate(env, types, "isExternal", TypeCheckKind::kExternal) ||
      !DefineTypePredicate(env, types, "isDate", TypeCheckKind::kDate) ||
      !DefineTypePredicate(env, types, "isArgumentsObject", TypeCheckKind::kArgumentsObject) ||
      !DefineTypePredicate(env, types, "isBooleanObject", TypeCheckKind::kBooleanObject) ||
      !DefineTypePredicate(env, types, "isNumberObject", TypeCheckKind::kNumberObject) ||
      !DefineTypePredicate(env, types, "isStringObject", TypeCheckKind::kStringObject) ||
      !DefineTypePredicate(env, types, "isSymbolObject", TypeCheckKind::kSymbolObject) ||
      !DefineTypePredicate(env, types, "isBigIntObject", TypeCheckKind::kBigIntObject) ||
      !DefineTypePredicate(env, types, "isNativeError", TypeCheckKind::kNativeError) ||
      !DefineTypePredicate(env, types, "isRegExp", TypeCheckKind::kRegExp) ||
      !DefineTypePredicate(env, types, "isAsyncFunction", TypeCheckKind::kAsyncFunction) ||
      !DefineTypePredicate(env, types, "isGeneratorFunction", TypeCheckKind::kGeneratorFunction) ||
      !DefineTypePredicate(env, types, "isGeneratorObject", TypeCheckKind::kGeneratorObject) ||
      !DefineTypePredicate(env, types, "isPromise", TypeCheckKind::kPromise) ||
      !DefineTypePredicate(env, types, "isMap", TypeCheckKind::kMap) ||
      !DefineTypePredicate(env, types, "isSet", TypeCheckKind::kSet) ||
      !DefineTypePredicate(env, types, "isMapIterator", TypeCheckKind::kMapIterator) ||
      !DefineTypePredicate(env, types, "isSetIterator", TypeCheckKind::kSetIterator) ||
      !DefineTypePredicate(env, types, "isWeakMap", TypeCheckKind::kWeakMap) ||
      !DefineTypePredicate(env, types, "isWeakSet", TypeCheckKind::kWeakSet) ||
      !DefineTypePredicate(env, types, "isArrayBuffer", TypeCheckKind::kArrayBuffer) ||
      !DefineTypePredicate(env, types, "isDataView", TypeCheckKind::kDataView) ||
      !DefineTypePredicate(env, types, "isSharedArrayBuffer", TypeCheckKind::kSharedArrayBuffer) ||
      !DefineTypePredicate(env, types, "isProxy", TypeCheckKind::kProxy) ||
      !DefineTypePredicate(env, types, "isModuleNamespaceObject", TypeCheckKind::kModuleNamespaceObject) ||
      !DefineTypePredicate(env, types, "isAnyArrayBuffer", TypeCheckKind::kAnyArrayBuffer) ||
      !DefineTypePredicate(env, types, "isBoxedPrimitive", TypeCheckKind::kBoxedPrimitive)) {
    return false;
  }

  auto it = g_types_binding_refs.find(env);
  if (it != g_types_binding_refs.end() && it->second != nullptr) {
    napi_delete_reference(env, it->second);
    it->second = nullptr;
  }
  napi_ref ref = nullptr;
  if (napi_create_reference(env, types, 1, &ref) != napi_ok || ref == nullptr) return false;
  g_types_binding_refs[env] = ref;
  return true;
}

}  // namespace

napi_value UbiInstallUtilBinding(napi_env env) {
  napi_value binding = nullptr;
  if (napi_create_object(env, &binding) != napi_ok || binding == nullptr) return nullptr;

  if (!InstallPrivateSymbols(env, binding) || !InstallConstants(env, binding) ||
      !InstallShouldAbortToggle(env, binding)) {
    return nullptr;
  }

  if (!DefineMethod(env, binding, "isInsideNodeModules", IsInsideNodeModulesCallback) ||
      !DefineMethod(env, binding, "defineLazyProperties", DefineLazyPropertiesCallback) ||
      !DefineMethod(env, binding, "getPromiseDetails", GetPromiseDetailsCallback) ||
      !DefineMethod(env, binding, "getProxyDetails", GetProxyDetailsCallback) ||
      !DefineMethod(env, binding, "getCallerLocation", GetCallerLocationCallback) ||
      !DefineMethod(env, binding, "previewEntries", PreviewEntriesCallback) ||
      !DefineMethod(env, binding, "getOwnNonIndexProperties", GetOwnNonIndexPropertiesCallback) ||
      !DefineMethod(env, binding, "getConstructorName", GetConstructorNameCallback) ||
      !DefineMethod(env, binding, "getExternalValue", GetExternalValueCallback) ||
      !DefineMethod(env, binding, "getCallSites", GetCallSitesCallback) ||
      !DefineMethod(env, binding, "sleep", SleepCallback) ||
      !DefineMethod(env, binding, "parseEnv", ParseEnvCallback) ||
      !DefineMethod(env, binding, "arrayBufferViewHasBuffer", ArrayBufferViewHasBufferCallback) ||
      !DefineMethod(env, binding, "constructSharedArrayBuffer", ConstructSharedArrayBufferCallback) ||
      !DefineMethod(env, binding, "guessHandleType", GuessHandleType)) {
    return nullptr;
  }

  if (!InstallTypesBinding(env)) return nullptr;
  return binding;
}

napi_value UbiGetTypesBinding(napi_env env) {
  auto it = g_types_binding_refs.find(env);
  if (it == g_types_binding_refs.end()) return nullptr;
  return GetRefValue(env, it->second);
}
