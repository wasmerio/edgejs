#include "internal_binding/dispatch.h"

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include "internal_binding/helpers.h"

namespace internal_binding {

namespace {

bool ReadByteSpan(napi_env env, napi_value value, const uint8_t** data, size_t* length) {
  if (value == nullptr || data == nullptr || length == nullptr) return false;

  bool is_buffer = false;
  if (napi_is_buffer(env, value, &is_buffer) == napi_ok && is_buffer) {
    void* raw = nullptr;
    if (napi_get_buffer_info(env, value, &raw, length) != napi_ok || raw == nullptr) return false;
    *data = static_cast<const uint8_t*>(raw);
    return true;
  }

  bool is_typed_array = false;
  if (napi_is_typedarray(env, value, &is_typed_array) == napi_ok && is_typed_array) {
    napi_typedarray_type ta_type = napi_uint8_array;
    size_t ta_length = 0;
    void* ta_data = nullptr;
    napi_value arraybuffer = nullptr;
    size_t byte_offset = 0;
    if (napi_get_typedarray_info(env,
                                 value,
                                 &ta_type,
                                 &ta_length,
                                 &ta_data,
                                 &arraybuffer,
                                 &byte_offset) != napi_ok ||
        ta_data == nullptr) {
      return false;
    }
    *data = static_cast<const uint8_t*>(ta_data);
    *length = ta_length;
    return true;
  }

  bool is_arraybuffer = false;
  if (napi_is_arraybuffer(env, value, &is_arraybuffer) == napi_ok && is_arraybuffer) {
    void* raw = nullptr;
    if (napi_get_arraybuffer_info(env, value, &raw, length) != napi_ok || raw == nullptr) return false;
    *data = static_cast<const uint8_t*>(raw);
    return true;
  }

  return false;
}

napi_value DecodeLatin1Callback(napi_env env, napi_callback_info info) {
  size_t argc = 3;
  napi_value argv[3] = {nullptr, nullptr, nullptr};
  if (napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr) != napi_ok || argc < 1 || argv[0] == nullptr) {
    return Undefined(env);
  }

  const uint8_t* data = nullptr;
  size_t length = 0;
  if (!ReadByteSpan(env, argv[0], &data, &length)) {
    std::string text;
    size_t copied = 0;
    if (napi_get_value_string_utf8(env, argv[0], nullptr, 0, &copied) == napi_ok) {
      std::vector<char> buffer(copied + 1);
      size_t written = 0;
      if (napi_get_value_string_utf8(env, argv[0], buffer.data(), buffer.size(), &written) == napi_ok) {
        text.assign(buffer.data(), written);
      }
    }
    napi_value out = nullptr;
    napi_create_string_utf8(env, text.c_str(), text.size(), &out);
    return out != nullptr ? out : Undefined(env);
  }

  int64_t start = 0;
  int64_t end = static_cast<int64_t>(length);
  if (argc >= 2 && argv[1] != nullptr) {
    napi_get_value_int64(env, argv[1], &start);
  }
  if (argc >= 3 && argv[2] != nullptr) {
    napi_get_value_int64(env, argv[2], &end);
  }

  const int64_t max_len = static_cast<int64_t>(length);
  start = std::max<int64_t>(0, std::min<int64_t>(start, max_len));
  end = std::max<int64_t>(start, std::min<int64_t>(end, max_len));

  napi_value out = nullptr;
  napi_create_string_latin1(env,
                            reinterpret_cast<const char*>(data + start),
                            static_cast<size_t>(end - start),
                            &out);
  return out != nullptr ? out : Undefined(env);
}

void EnsureDecodeLatin1(napi_env env, napi_value binding) {
  bool has_decode = false;
  if (napi_has_named_property(env, binding, "decodeLatin1", &has_decode) != napi_ok || !has_decode) {
    napi_value fn = nullptr;
    if (napi_create_function(env,
                             "decodeLatin1",
                             NAPI_AUTO_LENGTH,
                             DecodeLatin1Callback,
                             nullptr,
                             &fn) == napi_ok &&
        fn != nullptr) {
      napi_set_named_property(env, binding, "decodeLatin1", fn);
    }
    return;
  }

  napi_value decode = nullptr;
  if (napi_get_named_property(env, binding, "decodeLatin1", &decode) != napi_ok || decode == nullptr) return;
  napi_valuetype type = napi_undefined;
  if (napi_typeof(env, decode, &type) != napi_ok || type != napi_function) {
    napi_value fn = nullptr;
    if (napi_create_function(env,
                             "decodeLatin1",
                             NAPI_AUTO_LENGTH,
                             DecodeLatin1Callback,
                             nullptr,
                             &fn) == napi_ok &&
        fn != nullptr) {
      napi_set_named_property(env, binding, "decodeLatin1", fn);
    }
  }
}

}  // namespace

napi_value ResolveEncodingBinding(napi_env env, const ResolveOptions& options) {
  if (options.callbacks.resolve_binding == nullptr) return Undefined(env);
  napi_value binding = options.callbacks.resolve_binding(env, options.state, "encoding_binding");
  if (binding == nullptr || IsUndefined(env, binding)) return Undefined(env);
  EnsureDecodeLatin1(env, binding);
  return binding;
}

}  // namespace internal_binding
