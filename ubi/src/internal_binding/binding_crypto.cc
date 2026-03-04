#include "internal_binding/dispatch.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "internal_binding/helpers.h"

namespace internal_binding {

namespace {

constexpr int32_t kCryptoJobAsync = 0;
constexpr int32_t kCryptoJobSync = 1;
constexpr int32_t kSignJobModeSign = 0;
constexpr int32_t kSignJobModeVerify = 1;
constexpr int32_t kKeyTypeSecret = 0;
constexpr int32_t kKeyTypePublic = 1;
constexpr int32_t kKeyTypePrivate = 2;

struct CryptoBindingState {
  napi_ref binding_ref = nullptr;
  int32_t fips_mode = 0;
};

std::unordered_map<napi_env, CryptoBindingState> g_crypto_states;

CryptoBindingState* GetState(napi_env env) {
  auto it = g_crypto_states.find(env);
  if (it == g_crypto_states.end()) return nullptr;
  return &it->second;
}

napi_value GetRefValue(napi_env env, napi_ref ref) {
  if (ref == nullptr) return nullptr;
  napi_value out = nullptr;
  if (napi_get_reference_value(env, ref, &out) != napi_ok || out == nullptr) return nullptr;
  return out;
}

napi_value GetBinding(napi_env env) {
  CryptoBindingState* state = GetState(env);
  return state == nullptr ? nullptr : GetRefValue(env, state->binding_ref);
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

void SetNamedBool(napi_env env, napi_value obj, const char* name, bool value) {
  napi_value v = nullptr;
  if (napi_get_boolean(env, value, &v) == napi_ok && v != nullptr) napi_set_named_property(env, obj, name, v);
}

bool GetByteSpan(napi_env env, napi_value value, const uint8_t** data, size_t* length) {
  if (value == nullptr || data == nullptr || length == nullptr) return false;
  *data = nullptr;
  *length = 0;

  bool is_buffer = false;
  if (napi_is_buffer(env, value, &is_buffer) == napi_ok && is_buffer) {
    void* raw = nullptr;
    if (napi_get_buffer_info(env, value, &raw, length) != napi_ok || raw == nullptr) return false;
    *data = static_cast<const uint8_t*>(raw);
    return true;
  }

  bool is_typed_array = false;
  if (napi_is_typedarray(env, value, &is_typed_array) == napi_ok && is_typed_array) {
    napi_typedarray_type type = napi_uint8_array;
    size_t len = 0;
    void* raw = nullptr;
    napi_value arraybuffer = nullptr;
    size_t offset = 0;
    if (napi_get_typedarray_info(env, value, &type, &len, &raw, &arraybuffer, &offset) != napi_ok || raw == nullptr) {
      return false;
    }
    *data = static_cast<const uint8_t*>(raw);
    *length = len;
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

std::vector<uint8_t> ValueToBytes(napi_env env, napi_value value) {
  std::vector<uint8_t> out;
  const uint8_t* span = nullptr;
  size_t span_len = 0;
  if (GetByteSpan(env, value, &span, &span_len)) {
    out.assign(span, span + span_len);
    return out;
  }

  if (value != nullptr) {
    napi_value export_fn = nullptr;
    napi_valuetype t = napi_undefined;
    if (napi_get_named_property(env, value, "export", &export_fn) == napi_ok &&
        export_fn != nullptr &&
        napi_typeof(env, export_fn, &t) == napi_ok &&
        t == napi_function) {
      napi_value exported = nullptr;
      if (napi_call_function(env, value, export_fn, 0, nullptr, &exported) == napi_ok && exported != nullptr) {
        return ValueToBytes(env, exported);
      }
    }
  }

  size_t len = 0;
  if (napi_get_value_string_utf8(env, value, nullptr, 0, &len) == napi_ok) {
    std::string text(len + 1, '\0');
    size_t copied = 0;
    if (napi_get_value_string_utf8(env, value, text.data(), text.size(), &copied) == napi_ok) {
      text.resize(copied);
      out.assign(text.begin(), text.end());
      return out;
    }
  }

  return out;
}

napi_value BytesToBuffer(napi_env env, const std::vector<uint8_t>& data) {
  napi_value out = nullptr;
  void* copied = nullptr;
  if (napi_create_buffer_copy(env, data.size(), data.empty() ? nullptr : data.data(), &copied, &out) != napi_ok ||
      out == nullptr) {
    return Undefined(env);
  }
  return out;
}

napi_value EnsureBufferValue(napi_env env, napi_value value) {
  bool is_buffer = false;
  if (value != nullptr && napi_is_buffer(env, value, &is_buffer) == napi_ok && is_buffer) return value;
  return BytesToBuffer(env, ValueToBytes(env, value));
}

napi_value NormalizeToBufferObject(napi_env env, napi_value value) {
  napi_value global = GetGlobal(env);
  napi_value buffer_ctor = nullptr;
  napi_value from_fn = nullptr;
  napi_valuetype type = napi_undefined;
  if (global == nullptr ||
      napi_get_named_property(env, global, "Buffer", &buffer_ctor) != napi_ok ||
      buffer_ctor == nullptr ||
      napi_get_named_property(env, buffer_ctor, "from", &from_fn) != napi_ok ||
      from_fn == nullptr ||
      napi_typeof(env, from_fn, &type) != napi_ok ||
      type != napi_function) {
    return EnsureBufferValue(env, value);
  }
  napi_value argv[1] = {value != nullptr ? value : Undefined(env)};
  napi_value out = nullptr;
  if (napi_call_function(env, buffer_ctor, from_fn, 1, argv, &out) != napi_ok || out == nullptr) {
    return EnsureBufferValue(env, value);
  }
  return out;
}

bool CallBindingMethod(napi_env env,
                       napi_value binding,
                       const char* method,
                       size_t argc,
                       napi_value* argv,
                       napi_value* out) {
  if (binding == nullptr || method == nullptr || out == nullptr) return false;
  *out = nullptr;
  napi_value fn = nullptr;
  if (napi_get_named_property(env, binding, method, &fn) != napi_ok || fn == nullptr) return false;
  napi_valuetype t = napi_undefined;
  if (napi_typeof(env, fn, &t) != napi_ok || t != napi_function) return false;
  return napi_call_function(env, binding, fn, argc, argv, out) == napi_ok && *out != nullptr;
}

napi_value MaybeToEncodedOutput(napi_env env, napi_value buffer_value, napi_value encoding_value) {
  if (buffer_value == nullptr) return Undefined(env);
  napi_value as_buffer = NormalizeToBufferObject(env, buffer_value);
  if (encoding_value == nullptr || IsUndefined(env, encoding_value)) return as_buffer;

  size_t len = 0;
  if (napi_get_value_string_utf8(env, encoding_value, nullptr, 0, &len) != napi_ok) return buffer_value;
  std::string encoding(len + 1, '\0');
  size_t copied = 0;
  if (napi_get_value_string_utf8(env, encoding_value, encoding.data(), encoding.size(), &copied) != napi_ok) {
    return buffer_value;
  }
  encoding.resize(copied);
  if (encoding == "buffer" || encoding.empty()) return as_buffer;

  napi_value to_string_fn = nullptr;
  napi_valuetype type = napi_undefined;
  if (napi_get_named_property(env, as_buffer, "toString", &to_string_fn) != napi_ok ||
      to_string_fn == nullptr ||
      napi_typeof(env, to_string_fn, &type) != napi_ok ||
      type != napi_function) {
    return as_buffer;
  }
  napi_value enc = nullptr;
  if (napi_create_string_utf8(env, encoding.c_str(), NAPI_AUTO_LENGTH, &enc) != napi_ok || enc == nullptr) {
    return as_buffer;
  }
  napi_value out = nullptr;
  if (napi_call_function(env, as_buffer, to_string_fn, 1, &enc, &out) != napi_ok || out == nullptr) {
    return as_buffer;
  }
  return out;
}

struct HashWrap {
  napi_ref wrapper_ref = nullptr;
  std::string algorithm;
  std::vector<uint8_t> data;
};

void HashFinalize(napi_env env, void* data, void* /*hint*/) {
  auto* wrap = static_cast<HashWrap*>(data);
  if (wrap == nullptr) return;
  ResetRef(env, &wrap->wrapper_ref);
  delete wrap;
}

HashWrap* UnwrapHash(napi_env env, napi_value this_arg) {
  if (this_arg == nullptr) return nullptr;
  void* data = nullptr;
  if (napi_unwrap(env, this_arg, &data) != napi_ok || data == nullptr) return nullptr;
  return static_cast<HashWrap*>(data);
}

napi_value HashCtor(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  napi_value this_arg = nullptr;
  if (napi_get_cb_info(env, info, &argc, argv, &this_arg, nullptr) != napi_ok || this_arg == nullptr) {
    return nullptr;
  }
  auto* wrap = new HashWrap();
  if (argc >= 1 && argv[0] != nullptr) {
    size_t len = 0;
    if (napi_get_value_string_utf8(env, argv[0], nullptr, 0, &len) == napi_ok) {
      wrap->algorithm.resize(len + 1);
      size_t copied = 0;
      if (napi_get_value_string_utf8(env, argv[0], wrap->algorithm.data(), wrap->algorithm.size(), &copied) == napi_ok) {
        wrap->algorithm.resize(copied);
      }
    }
  }
  if (wrap->algorithm.empty()) wrap->algorithm = "sha256";
  napi_wrap(env, this_arg, wrap, HashFinalize, nullptr, &wrap->wrapper_ref);
  return this_arg;
}

napi_value HashUpdate(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  napi_value this_arg = nullptr;
  napi_get_cb_info(env, info, &argc, argv, &this_arg, nullptr);
  HashWrap* wrap = UnwrapHash(env, this_arg);
  if (wrap == nullptr) return this_arg != nullptr ? this_arg : Undefined(env);
  if (argc >= 1 && argv[0] != nullptr) {
    std::vector<uint8_t> input = ValueToBytes(env, argv[0]);
    wrap->data.insert(wrap->data.end(), input.begin(), input.end());
  }
  return this_arg != nullptr ? this_arg : Undefined(env);
}

napi_value HashDigest(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  napi_value this_arg = nullptr;
  napi_get_cb_info(env, info, &argc, argv, &this_arg, nullptr);
  HashWrap* wrap = UnwrapHash(env, this_arg);
  if (wrap == nullptr) return Undefined(env);
  napi_value binding = GetBinding(env);
  if (binding == nullptr) return Undefined(env);

  napi_value algorithm = nullptr;
  napi_create_string_utf8(env, wrap->algorithm.c_str(), NAPI_AUTO_LENGTH, &algorithm);
  napi_value data = BytesToBuffer(env, wrap->data);
  napi_value call_argv[2] = {algorithm, data};
  napi_value out = nullptr;
  if (!CallBindingMethod(env, binding, "hashOneShot", 2, call_argv, &out)) return Undefined(env);
  napi_value as_buffer = EnsureBufferValue(env, out);
  wrap->data.clear();
  return MaybeToEncodedOutput(env, as_buffer, argc >= 1 ? argv[0] : nullptr);
}

struct HmacWrap {
  napi_ref wrapper_ref = nullptr;
  std::string algorithm;
  std::vector<uint8_t> key;
  std::vector<uint8_t> data;
};

void HmacFinalize(napi_env env, void* data, void* /*hint*/) {
  auto* wrap = static_cast<HmacWrap*>(data);
  if (wrap == nullptr) return;
  ResetRef(env, &wrap->wrapper_ref);
  delete wrap;
}

HmacWrap* UnwrapHmac(napi_env env, napi_value this_arg) {
  if (this_arg == nullptr) return nullptr;
  void* data = nullptr;
  if (napi_unwrap(env, this_arg, &data) != napi_ok || data == nullptr) return nullptr;
  return static_cast<HmacWrap*>(data);
}

napi_value HmacCtor(napi_env env, napi_callback_info info) {
  napi_value this_arg = nullptr;
  size_t argc = 0;
  napi_get_cb_info(env, info, &argc, nullptr, &this_arg, nullptr);
  if (this_arg == nullptr) return nullptr;
  auto* wrap = new HmacWrap();
  wrap->algorithm = "sha256";
  napi_wrap(env, this_arg, wrap, HmacFinalize, nullptr, &wrap->wrapper_ref);
  return this_arg;
}

napi_value HmacInit(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value argv[2] = {nullptr, nullptr};
  napi_value this_arg = nullptr;
  napi_get_cb_info(env, info, &argc, argv, &this_arg, nullptr);
  HmacWrap* wrap = UnwrapHmac(env, this_arg);
  if (wrap == nullptr) return Undefined(env);
  if (argc >= 1 && argv[0] != nullptr) {
    size_t len = 0;
    if (napi_get_value_string_utf8(env, argv[0], nullptr, 0, &len) == napi_ok) {
      wrap->algorithm.resize(len + 1);
      size_t copied = 0;
      if (napi_get_value_string_utf8(env, argv[0], wrap->algorithm.data(), wrap->algorithm.size(), &copied) == napi_ok) {
        wrap->algorithm.resize(copied);
      }
    }
  }
  wrap->key = (argc >= 2 && argv[1] != nullptr) ? ValueToBytes(env, argv[1]) : std::vector<uint8_t>{};
  wrap->data.clear();
  return Undefined(env);
}

napi_value HmacUpdate(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  napi_value this_arg = nullptr;
  napi_get_cb_info(env, info, &argc, argv, &this_arg, nullptr);
  HmacWrap* wrap = UnwrapHmac(env, this_arg);
  if (wrap == nullptr) return this_arg != nullptr ? this_arg : Undefined(env);
  if (argc >= 1 && argv[0] != nullptr) {
    std::vector<uint8_t> bytes = ValueToBytes(env, argv[0]);
    wrap->data.insert(wrap->data.end(), bytes.begin(), bytes.end());
  }
  return this_arg != nullptr ? this_arg : Undefined(env);
}

napi_value HmacDigest(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  napi_value this_arg = nullptr;
  napi_get_cb_info(env, info, &argc, argv, &this_arg, nullptr);
  HmacWrap* wrap = UnwrapHmac(env, this_arg);
  if (wrap == nullptr) return Undefined(env);
  napi_value binding = GetBinding(env);
  if (binding == nullptr) return Undefined(env);

  napi_value algorithm = nullptr;
  napi_create_string_utf8(env, wrap->algorithm.c_str(), NAPI_AUTO_LENGTH, &algorithm);
  napi_value key = BytesToBuffer(env, wrap->key);
  napi_value data = BytesToBuffer(env, wrap->data);
  napi_value call_argv[3] = {algorithm, key, data};
  napi_value out = nullptr;
  if (!CallBindingMethod(env, binding, "hmacOneShot", 3, call_argv, &out)) return Undefined(env);
  napi_value as_buffer = EnsureBufferValue(env, out);
  wrap->data.clear();
  return MaybeToEncodedOutput(env, as_buffer, argc >= 1 ? argv[0] : nullptr);
}

struct KeyObjectWrap {
  napi_ref wrapper_ref = nullptr;
  int32_t key_type = kKeyTypeSecret;
  std::vector<uint8_t> key_data;
};

void KeyObjectFinalize(napi_env env, void* data, void* /*hint*/) {
  auto* wrap = static_cast<KeyObjectWrap*>(data);
  if (wrap == nullptr) return;
  ResetRef(env, &wrap->wrapper_ref);
  delete wrap;
}

KeyObjectWrap* UnwrapKeyObject(napi_env env, napi_value this_arg) {
  if (this_arg == nullptr) return nullptr;
  void* data = nullptr;
  if (napi_unwrap(env, this_arg, &data) != napi_ok || data == nullptr) return nullptr;
  return static_cast<KeyObjectWrap*>(data);
}

napi_value KeyObjectCtor(napi_env env, napi_callback_info info) {
  napi_value this_arg = nullptr;
  size_t argc = 0;
  napi_get_cb_info(env, info, &argc, nullptr, &this_arg, nullptr);
  if (this_arg == nullptr) return nullptr;
  auto* wrap = new KeyObjectWrap();
  napi_wrap(env, this_arg, wrap, KeyObjectFinalize, nullptr, &wrap->wrapper_ref);
  return this_arg;
}

napi_value KeyObjectInit(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value argv[2] = {nullptr, nullptr};
  napi_value this_arg = nullptr;
  napi_get_cb_info(env, info, &argc, argv, &this_arg, nullptr);
  KeyObjectWrap* wrap = UnwrapKeyObject(env, this_arg);
  if (wrap == nullptr) return Undefined(env);
  int32_t key_type = kKeyTypeSecret;
  if (argc >= 1 && argv[0] != nullptr) napi_get_value_int32(env, argv[0], &key_type);
  wrap->key_type = key_type;
  wrap->key_data = (argc >= 2 && argv[1] != nullptr) ? ValueToBytes(env, argv[1]) : std::vector<uint8_t>{};
  napi_value out = nullptr;
  napi_get_boolean(env, true, &out);
  return out != nullptr ? out : Undefined(env);
}

napi_value KeyObjectExport(napi_env env, napi_callback_info info) {
  napi_value this_arg = nullptr;
  size_t argc = 0;
  napi_get_cb_info(env, info, &argc, nullptr, &this_arg, nullptr);
  KeyObjectWrap* wrap = UnwrapKeyObject(env, this_arg);
  if (wrap == nullptr) return Undefined(env);
  return BytesToBuffer(env, wrap->key_data);
}

napi_value KeyObjectGetSymmetricKeySize(napi_env env, napi_callback_info info) {
  napi_value this_arg = nullptr;
  size_t argc = 0;
  napi_get_cb_info(env, info, &argc, nullptr, &this_arg, nullptr);
  KeyObjectWrap* wrap = UnwrapKeyObject(env, this_arg);
  uint32_t size = wrap == nullptr ? 0 : static_cast<uint32_t>(wrap->key_data.size());
  napi_value out = nullptr;
  napi_create_uint32(env, size, &out);
  return out != nullptr ? out : Undefined(env);
}

napi_value KeyObjectEquals(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  napi_value this_arg = nullptr;
  napi_get_cb_info(env, info, &argc, argv, &this_arg, nullptr);
  KeyObjectWrap* left = UnwrapKeyObject(env, this_arg);
  bool equal = false;
  if (left != nullptr && argc >= 1 && argv[0] != nullptr) {
    KeyObjectWrap* right = UnwrapKeyObject(env, argv[0]);
    if (right != nullptr && left->key_type == right->key_type && left->key_data.size() == right->key_data.size()) {
      equal = std::memcmp(left->key_data.data(), right->key_data.data(), left->key_data.size()) == 0;
    } else {
      std::vector<uint8_t> exported = ValueToBytes(env, argv[0]);
      equal = exported.size() == left->key_data.size() &&
              std::memcmp(exported.data(), left->key_data.data(), left->key_data.size()) == 0;
    }
  }
  napi_value out = nullptr;
  napi_get_boolean(env, equal, &out);
  return out != nullptr ? out : Undefined(env);
}

struct SecureContextWrap {
  napi_ref wrapper_ref = nullptr;
  napi_ref handle_ref = nullptr;
  int32_t min_proto = 0;
  int32_t max_proto = 0;
};

void SecureContextFinalize(napi_env env, void* data, void* /*hint*/) {
  auto* wrap = static_cast<SecureContextWrap*>(data);
  if (wrap == nullptr) return;
  ResetRef(env, &wrap->wrapper_ref);
  ResetRef(env, &wrap->handle_ref);
  delete wrap;
}

SecureContextWrap* UnwrapSecureContext(napi_env env, napi_value this_arg) {
  if (this_arg == nullptr) return nullptr;
  void* data = nullptr;
  if (napi_unwrap(env, this_arg, &data) != napi_ok || data == nullptr) return nullptr;
  return static_cast<SecureContextWrap*>(data);
}

napi_value SecureContextCtor(napi_env env, napi_callback_info info) {
  napi_value this_arg = nullptr;
  size_t argc = 0;
  napi_get_cb_info(env, info, &argc, nullptr, &this_arg, nullptr);
  if (this_arg == nullptr) return nullptr;
  auto* wrap = new SecureContextWrap();
  napi_wrap(env, this_arg, wrap, SecureContextFinalize, nullptr, &wrap->wrapper_ref);
  return this_arg;
}

napi_value SecureContextInit(napi_env env, napi_callback_info info) {
  size_t argc = 3;
  napi_value argv[3] = {nullptr, nullptr, nullptr};
  napi_value this_arg = nullptr;
  napi_get_cb_info(env, info, &argc, argv, &this_arg, nullptr);
  SecureContextWrap* wrap = UnwrapSecureContext(env, this_arg);
  if (wrap == nullptr) return Undefined(env);
  if (argc >= 2 && argv[1] != nullptr) napi_get_value_int32(env, argv[1], &wrap->min_proto);
  if (argc >= 3 && argv[2] != nullptr) napi_get_value_int32(env, argv[2], &wrap->max_proto);

  napi_value binding = GetBinding(env);
  if (binding != nullptr) {
    napi_value handle = nullptr;
    if (CallBindingMethod(env, binding, "secureContextCreate", 0, nullptr, &handle)) {
      ResetRef(env, &wrap->handle_ref);
      napi_create_reference(env, handle, 1, &wrap->handle_ref);
      napi_value init_argv[4] = {handle, argc >= 1 ? argv[0] : Undefined(env), argc >= 2 ? argv[1] : Undefined(env),
                                 argc >= 3 ? argv[2] : Undefined(env)};
      napi_value ignored = nullptr;
      CallBindingMethod(env, binding, "secureContextInit", 4, init_argv, &ignored);
    }
  }
  return Undefined(env);
}

napi_value SecureContextSetOptions(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  napi_value this_arg = nullptr;
  napi_get_cb_info(env, info, &argc, argv, &this_arg, nullptr);
  SecureContextWrap* wrap = UnwrapSecureContext(env, this_arg);
  if (wrap == nullptr) return Undefined(env);
  napi_value binding = GetBinding(env);
  napi_value handle = GetRefValue(env, wrap->handle_ref);
  if (binding != nullptr && handle != nullptr) {
    napi_value call_argv[2] = {handle, argc >= 1 ? argv[0] : Undefined(env)};
    napi_value ignored = nullptr;
    CallBindingMethod(env, binding, "secureContextSetOptions", 2, call_argv, &ignored);
  }
  return Undefined(env);
}

napi_value SecureContextGetMinProto(napi_env env, napi_callback_info info) {
  napi_value this_arg = nullptr;
  size_t argc = 0;
  napi_get_cb_info(env, info, &argc, nullptr, &this_arg, nullptr);
  SecureContextWrap* wrap = UnwrapSecureContext(env, this_arg);
  napi_value out = nullptr;
  napi_create_int32(env, wrap == nullptr ? 0 : wrap->min_proto, &out);
  return out != nullptr ? out : Undefined(env);
}

napi_value SecureContextGetMaxProto(napi_env env, napi_callback_info info) {
  napi_value this_arg = nullptr;
  size_t argc = 0;
  napi_get_cb_info(env, info, &argc, nullptr, &this_arg, nullptr);
  SecureContextWrap* wrap = UnwrapSecureContext(env, this_arg);
  napi_value out = nullptr;
  napi_create_int32(env, wrap == nullptr ? 0 : wrap->max_proto, &out);
  return out != nullptr ? out : Undefined(env);
}

napi_value SecureContextClose(napi_env env, napi_callback_info info) {
  napi_value this_arg = nullptr;
  size_t argc = 0;
  napi_get_cb_info(env, info, &argc, nullptr, &this_arg, nullptr);
  SecureContextWrap* wrap = UnwrapSecureContext(env, this_arg);
  if (wrap != nullptr) ResetRef(env, &wrap->handle_ref);
  return Undefined(env);
}

struct JobWrap {
  napi_ref wrapper_ref = nullptr;
  int32_t mode = kCryptoJobSync;
  std::vector<napi_ref> args;
};

void JobFinalize(napi_env env, void* data, void* /*hint*/) {
  auto* wrap = static_cast<JobWrap*>(data);
  if (wrap == nullptr) return;
  for (auto& arg : wrap->args) ResetRef(env, &arg);
  ResetRef(env, &wrap->wrapper_ref);
  delete wrap;
}

JobWrap* UnwrapJob(napi_env env, napi_value this_arg) {
  if (this_arg == nullptr) return nullptr;
  void* data = nullptr;
  if (napi_unwrap(env, this_arg, &data) != napi_ok || data == nullptr) return nullptr;
  return static_cast<JobWrap*>(data);
}

napi_value JobCtor(napi_env env, napi_callback_info info) {
  size_t argc = 16;
  napi_value argv[16] = {nullptr};
  napi_value this_arg = nullptr;
  napi_get_cb_info(env, info, &argc, argv, &this_arg, nullptr);
  if (this_arg == nullptr) return nullptr;
  auto* wrap = new JobWrap();
  if (argc >= 1 && argv[0] != nullptr) napi_get_value_int32(env, argv[0], &wrap->mode);
  for (size_t i = 1; i < argc; ++i) {
    napi_ref ref = nullptr;
    if (argv[i] != nullptr) napi_create_reference(env, argv[i], 1, &ref);
    wrap->args.push_back(ref);
  }
  napi_wrap(env, this_arg, wrap, JobFinalize, nullptr, &wrap->wrapper_ref);
  return this_arg;
}

napi_value BuildJobResult(napi_env env, napi_value err, napi_value value) {
  napi_value out = nullptr;
  napi_create_array_with_length(env, 2, &out);
  if (out == nullptr) return Undefined(env);
  napi_set_element(env, out, 0, err != nullptr ? err : Undefined(env));
  napi_set_element(env, out, 1, value != nullptr ? value : Undefined(env));
  return out;
}

napi_value RunSyncCall(napi_env env, napi_value this_arg, const char* method, std::vector<napi_value> call_args) {
  napi_value binding = GetBinding(env);
  if (binding == nullptr) return BuildJobResult(env, nullptr, Undefined(env));
  napi_value result = nullptr;
  if (!CallBindingMethod(env, binding, method, call_args.size(), call_args.data(), &result)) {
    bool pending = false;
    if (napi_is_exception_pending(env, &pending) == napi_ok && pending) {
      napi_value err = nullptr;
      napi_get_and_clear_last_exception(env, &err);
      return BuildJobResult(env, err, Undefined(env));
    }
    return BuildJobResult(env, Undefined(env), Undefined(env));
  }
  return BuildJobResult(env, nullptr, result);
}

napi_value RandomBytesJobRun(napi_env env, napi_callback_info info) {
  napi_value this_arg = nullptr;
  size_t argc = 0;
  napi_get_cb_info(env, info, &argc, nullptr, &this_arg, nullptr);
  JobWrap* wrap = UnwrapJob(env, this_arg);
  if (wrap == nullptr || wrap->args.size() < 3) return BuildJobResult(env, nullptr, Undefined(env));
  napi_value buffer = GetRefValue(env, wrap->args[0]);
  napi_value offset = GetRefValue(env, wrap->args[1]);
  napi_value size = GetRefValue(env, wrap->args[2]);
  std::vector<napi_value> argsv = {buffer != nullptr ? buffer : Undefined(env),
                                   offset != nullptr ? offset : Undefined(env),
                                   size != nullptr ? size : Undefined(env)};
  napi_value result = RunSyncCall(env, this_arg, "randomFillSync", argsv);
  return wrap->mode == kCryptoJobAsync ? Undefined(env) : result;
}

napi_value PBKDF2JobRun(napi_env env, napi_callback_info info) {
  napi_value this_arg = nullptr;
  size_t argc = 0;
  napi_get_cb_info(env, info, &argc, nullptr, &this_arg, nullptr);
  JobWrap* wrap = UnwrapJob(env, this_arg);
  if (wrap == nullptr || wrap->args.size() < 5) return BuildJobResult(env, nullptr, Undefined(env));
  std::vector<napi_value> argsv;
  for (size_t i = 0; i < 5; ++i) argsv.push_back(GetRefValue(env, wrap->args[i]));
  napi_value result = RunSyncCall(env, this_arg, "pbkdf2Sync", argsv);
  return wrap->mode == kCryptoJobAsync ? Undefined(env) : result;
}

napi_value ScryptJobRun(napi_env env, napi_callback_info info) {
  napi_value this_arg = nullptr;
  size_t argc = 0;
  napi_get_cb_info(env, info, &argc, nullptr, &this_arg, nullptr);
  JobWrap* wrap = UnwrapJob(env, this_arg);
  if (wrap == nullptr || wrap->args.size() < 7) return BuildJobResult(env, nullptr, Undefined(env));
  napi_value password = GetRefValue(env, wrap->args[0]);
  napi_value salt = GetRefValue(env, wrap->args[1]);
  napi_value N = GetRefValue(env, wrap->args[2]);
  napi_value r = GetRefValue(env, wrap->args[3]);
  napi_value p = GetRefValue(env, wrap->args[4]);
  napi_value maxmem = GetRefValue(env, wrap->args[5]);
  napi_value keylen = GetRefValue(env, wrap->args[6]);
  std::vector<napi_value> argsv = {password, salt, keylen, N, r, p, maxmem};
  napi_value result = RunSyncCall(env, this_arg, "scryptSync", argsv);
  return wrap->mode == kCryptoJobAsync ? Undefined(env) : result;
}

napi_value HKDFJobRun(napi_env env, napi_callback_info info) {
  napi_value this_arg = nullptr;
  size_t argc = 0;
  napi_get_cb_info(env, info, &argc, nullptr, &this_arg, nullptr);
  JobWrap* wrap = UnwrapJob(env, this_arg);
  if (wrap == nullptr || wrap->args.size() < 5) return BuildJobResult(env, nullptr, Undefined(env));
  std::vector<napi_value> argsv;
  for (size_t i = 0; i < 5; ++i) argsv.push_back(GetRefValue(env, wrap->args[i]));
  napi_value result = RunSyncCall(env, this_arg, "hkdfSync", argsv);
  return wrap->mode == kCryptoJobAsync ? Undefined(env) : result;
}

napi_value HashJobRun(napi_env env, napi_callback_info info) {
  napi_value this_arg = nullptr;
  size_t argc = 0;
  napi_get_cb_info(env, info, &argc, nullptr, &this_arg, nullptr);
  JobWrap* wrap = UnwrapJob(env, this_arg);
  if (wrap == nullptr || wrap->args.size() < 2) return BuildJobResult(env, nullptr, Undefined(env));
  std::vector<napi_value> argsv;
  for (size_t i = 0; i < wrap->args.size(); ++i) argsv.push_back(GetRefValue(env, wrap->args[i]));
  const char* method = wrap->args.size() >= 3 ? "hashOneShotXof" : "hashOneShot";
  napi_value result = RunSyncCall(env, this_arg, method, argsv);
  return wrap->mode == kCryptoJobAsync ? Undefined(env) : result;
}

napi_value SignJobRun(napi_env env, napi_callback_info info) {
  napi_value this_arg = nullptr;
  size_t argc = 0;
  napi_get_cb_info(env, info, &argc, nullptr, &this_arg, nullptr);
  return BuildJobResult(env, nullptr, Undefined(env));
}

napi_value CryptoOneShotDigest(napi_env env, napi_callback_info info) {
  size_t argc = 7;
  napi_value argv[7] = {nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr};
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  napi_value binding = GetBinding(env);
  if (binding == nullptr || argc < 4) return Undefined(env);

  napi_value algorithm = argv[0];
  napi_value input = EnsureBufferValue(env, argv[3]);
  bool has_output_length = argc >= 7 && argv[6] != nullptr && !IsUndefined(env, argv[6]);

  napi_value out = nullptr;
  if (has_output_length) {
    napi_value call_argv[3] = {algorithm, input, argv[6]};
    CallBindingMethod(env, binding, "hashOneShotXof", 3, call_argv, &out);
  }
  if (out == nullptr) {
    napi_value call_argv[2] = {algorithm, input};
    if (!CallBindingMethod(env, binding, "hashOneShot", 2, call_argv, &out)) return Undefined(env);
  }
  napi_value as_buffer = EnsureBufferValue(env, out);
  return MaybeToEncodedOutput(env, as_buffer, argc >= 5 ? argv[4] : nullptr);
}

napi_value CryptoTimingSafeEqual(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value argv[2] = {nullptr, nullptr};
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  if (argc < 2) return Undefined(env);

  std::vector<uint8_t> left = ValueToBytes(env, argv[0]);
  std::vector<uint8_t> right = ValueToBytes(env, argv[1]);
  if (left.size() != right.size()) {
    napi_throw_range_error(env, nullptr, "Input buffers must have the same byte length");
    return nullptr;
  }
  uint8_t diff = 0;
  for (size_t i = 0; i < left.size(); ++i) diff |= static_cast<uint8_t>(left[i] ^ right[i]);
  napi_value out = nullptr;
  napi_get_boolean(env, diff == 0, &out);
  return out != nullptr ? out : Undefined(env);
}

napi_value CryptoSecureBuffer(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  int64_t size = 0;
  if (argc >= 1 && argv[0] != nullptr) napi_get_value_int64(env, argv[0], &size);
  if (size < 0) size = 0;
  napi_value ab = nullptr;
  void* data = nullptr;
  if (napi_create_arraybuffer(env, static_cast<size_t>(size), &data, &ab) != napi_ok || ab == nullptr) {
    return Undefined(env);
  }
  if (data != nullptr) std::memset(data, 0, static_cast<size_t>(size));
  napi_value out = nullptr;
  napi_create_typedarray(env, napi_uint8_array, static_cast<size_t>(size), ab, 0, &out);
  return out != nullptr ? out : Undefined(env);
}

napi_value CryptoSecureHeapUsed(napi_env env, napi_callback_info info) {
  napi_value out = nullptr;
  if (napi_create_object(env, &out) != napi_ok || out == nullptr) return Undefined(env);
  SetNamedInt(env, out, "total", 0);
  SetNamedInt(env, out, "used", 0);
  SetNamedInt(env, out, "utilization", 0);
  SetNamedInt(env, out, "min", 0);
  return out;
}

napi_value CryptoGetCachedAliases(napi_env env, napi_callback_info info) {
  napi_value out = nullptr;
  if (napi_create_object(env, &out) != napi_ok || out == nullptr) return Undefined(env);
  napi_value binding = GetBinding(env);
  if (binding == nullptr) return out;
  napi_value hashes = nullptr;
  if (!CallBindingMethod(env, binding, "getHashes", 0, nullptr, &hashes) || hashes == nullptr) return out;
  bool is_array = false;
  if (napi_is_array(env, hashes, &is_array) != napi_ok || !is_array) return out;
  uint32_t len = 0;
  napi_get_array_length(env, hashes, &len);
  for (uint32_t i = 0; i < len; ++i) {
    napi_value key = nullptr;
    if (napi_get_element(env, hashes, i, &key) != napi_ok || key == nullptr) continue;
    napi_value value = nullptr;
    napi_create_uint32(env, i, &value);
    if (value != nullptr) napi_set_property(env, out, key, value);
  }
  return out;
}

napi_value CryptoGetFips(napi_env env, napi_callback_info info) {
  CryptoBindingState* state = GetState(env);
  napi_value out = nullptr;
  napi_create_int32(env, state == nullptr ? 0 : state->fips_mode, &out);
  return out != nullptr ? out : Undefined(env);
}

napi_value CryptoSetFips(napi_env env, napi_callback_info info) {
  CryptoBindingState* state = GetState(env);
  if (state == nullptr) return Undefined(env);
  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  bool enabled = false;
  if (argc >= 1 && argv[0] != nullptr) napi_get_value_bool(env, argv[0], &enabled);
  state->fips_mode = enabled ? 1 : 0;
  return Undefined(env);
}

napi_value CryptoTestFips(napi_env env, napi_callback_info info) {
  napi_value out = nullptr;
  napi_get_boolean(env, true, &out);
  return out != nullptr ? out : Undefined(env);
}

napi_value CryptoGetOpenSSLSecLevel(napi_env env, napi_callback_info info) {
  napi_value out = nullptr;
  napi_create_int32(env, 0, &out);
  return out != nullptr ? out : Undefined(env);
}

napi_value CryptoGetEmptyArray(napi_env env, napi_callback_info info) {
  napi_value out = nullptr;
  napi_create_array_with_length(env, 0, &out);
  return out != nullptr ? out : Undefined(env);
}

napi_value CryptoNoop(napi_env env, napi_callback_info info) {
  return Undefined(env);
}

napi_value CryptoGetSSLCiphers(napi_env env, napi_callback_info info) {
  napi_value binding = GetBinding(env);
  if (binding != nullptr) {
    napi_value out = nullptr;
    if (CallBindingMethod(env, binding, "getCiphers", 0, nullptr, &out) && out != nullptr) return out;
  }
  napi_value empty = nullptr;
  napi_create_array_with_length(env, 0, &empty);
  return empty != nullptr ? empty : Undefined(env);
}

napi_value CryptoParseX509(napi_env env, napi_callback_info info) {
  napi_value out = nullptr;
  if (napi_create_object(env, &out) != napi_ok || out == nullptr) return Undefined(env);
  SetNamedMethod(env, out, "subject", CryptoNoop);
  SetNamedMethod(env, out, "issuer", CryptoNoop);
  SetNamedMethod(env, out, "checkHost", CryptoNoop);
  SetNamedMethod(env, out, "verify", CryptoNoop);
  return out;
}

napi_value CryptoCreateNativeKeyObjectClass(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  if (argc < 1 || argv[0] == nullptr) return Undefined(env);
  napi_valuetype type = napi_undefined;
  if (napi_typeof(env, argv[0], &type) != napi_ok || type != napi_function) return Undefined(env);

  napi_property_descriptor props[] = {
      {"constructor", nullptr, nullptr, nullptr, nullptr, nullptr, napi_default, nullptr},
  };
  napi_value native_ctor = nullptr;
  auto ctor = [](napi_env env, napi_callback_info info) -> napi_value {
    size_t argc = 1;
    napi_value argv[1] = {nullptr};
    napi_value this_arg = nullptr;
    napi_get_cb_info(env, info, &argc, argv, &this_arg, nullptr);
    if (this_arg != nullptr && argc >= 1 && argv[0] != nullptr) {
      napi_set_named_property(env, this_arg, "handle", argv[0]);
    }
    return this_arg != nullptr ? this_arg : Undefined(env);
  };
  if (napi_define_class(env,
                        "NativeKeyObject",
                        NAPI_AUTO_LENGTH,
                        ctor,
                        nullptr,
                        sizeof(props) / sizeof(props[0]),
                        props,
                        &native_ctor) != napi_ok ||
      native_ctor == nullptr) {
    return Undefined(env);
  }

  napi_value global = GetGlobal(env);
  napi_value call_argv[1] = {native_ctor};
  napi_value out = nullptr;
  if (napi_call_function(env, global, argv[0], 1, call_argv, &out) != napi_ok || out == nullptr) {
    return Undefined(env);
  }
  return out;
}

void EnsurePropertyMethod(napi_env env, napi_value binding, const char* name, napi_callback cb) {
  bool has = false;
  if (napi_has_named_property(env, binding, name, &has) != napi_ok || has) return;
  SetNamedMethod(env, binding, name, cb);
}

napi_value CryptoUnsupportedMethod(napi_env env, napi_callback_info info) {
  napi_throw_error(env, "ERR_CRYPTO_UNSUPPORTED_OPERATION", "Unsupported crypto operation");
  return nullptr;
}

napi_value CryptoUnsupportedCtor(napi_env env, napi_callback_info info) {
  napi_throw_error(env, "ERR_CRYPTO_UNSUPPORTED_OPERATION", "Unsupported crypto constructor");
  return nullptr;
}

void EnsureUndefinedProperty(napi_env env, napi_value binding, const char* name) {
  bool has = false;
  if (napi_has_named_property(env, binding, name, &has) != napi_ok || has) return;
  napi_set_named_property(env, binding, name, Undefined(env));
}

void EnsureStubMethod(napi_env env, napi_value binding, const char* name) {
  bool has = false;
  if (napi_has_named_property(env, binding, name, &has) != napi_ok || has) return;
  SetNamedMethod(env, binding, name, CryptoUnsupportedMethod);
}

void EnsureStubClass(napi_env env, napi_value binding, const char* name) {
  bool has = false;
  if (napi_has_named_property(env, binding, name, &has) == napi_ok && has) return;
  napi_value cls = nullptr;
  if (napi_define_class(env, name, NAPI_AUTO_LENGTH, CryptoUnsupportedCtor, nullptr, 0, nullptr, &cls) == napi_ok &&
      cls != nullptr) {
    napi_set_named_property(env, binding, name, cls);
  }
}

void EnsureClass(napi_env env,
                 napi_value binding,
                 const char* name,
                 napi_callback ctor,
                 const std::vector<napi_property_descriptor>& methods) {
  bool has = false;
  if (napi_has_named_property(env, binding, name, &has) == napi_ok && has) return;
  napi_value cls = nullptr;
  if (napi_define_class(env,
                        name,
                        NAPI_AUTO_LENGTH,
                        ctor,
                        nullptr,
                        methods.size(),
                        methods.data(),
                        &cls) == napi_ok &&
      cls != nullptr) {
    napi_set_named_property(env, binding, name, cls);
  }
}

}  // namespace

napi_value ResolveCrypto(napi_env env, const ResolveOptions& options) {
  if (options.callbacks.resolve_binding == nullptr) return Undefined(env);
  napi_value out = options.callbacks.resolve_binding(env, options.state, "crypto");
  if (out == nullptr || IsUndefined(env, out)) return Undefined(env);

  auto& st = g_crypto_states[env];
  if (st.binding_ref == nullptr) napi_create_reference(env, out, 1, &st.binding_ref);

  // Core constants.
  const std::pair<const char*, int32_t> constants[] = {
      {"kCryptoJobAsync", kCryptoJobAsync},
      {"kCryptoJobSync", kCryptoJobSync},
      {"kSignJobModeSign", kSignJobModeSign},
      {"kSignJobModeVerify", kSignJobModeVerify},
      {"kWebCryptoCipherEncrypt", 0},
      {"kWebCryptoCipherDecrypt", 1},
      {"kKeyTypeSecret", kKeyTypeSecret},
      {"kKeyTypePublic", kKeyTypePublic},
      {"kKeyTypePrivate", kKeyTypePrivate},
      {"kKeyFormatDER", 0},
      {"kKeyFormatPEM", 1},
      {"kKeyFormatJWK", 2},
      {"kKeyEncodingPKCS1", 0},
      {"kKeyEncodingPKCS8", 1},
      {"kKeyEncodingSPKI", 2},
      {"kKeyEncodingSEC1", 3},
      {"kSigEncDER", 0},
      {"kSigEncP1363", 1},
      {"kKEMEncapsulate", 0},
      {"kKEMDecapsulate", 1},
      {"kTypeArgon2d", 0},
      {"kTypeArgon2i", 1},
      {"kTypeArgon2id", 2},
      {"kWebCryptoKeyFormatRaw", 0},
      {"kWebCryptoKeyFormatPKCS8", 1},
      {"kWebCryptoKeyFormatSPKI", 2},
      {"kWebCryptoKeyFormatJWK", 3},
      {"kKeyVariantRSA_SSA_PKCS1_v1_5", 0},
      {"kKeyVariantRSA_PSS", 1},
      {"kKeyVariantRSA_OAEP", 2},
      {"kKeyVariantAES_CTR_128", 0},
      {"kKeyVariantAES_CTR_192", 1},
      {"kKeyVariantAES_CTR_256", 2},
      {"kKeyVariantAES_CBC_128", 3},
      {"kKeyVariantAES_CBC_192", 4},
      {"kKeyVariantAES_CBC_256", 5},
      {"kKeyVariantAES_GCM_128", 6},
      {"kKeyVariantAES_GCM_192", 7},
      {"kKeyVariantAES_GCM_256", 8},
      {"kKeyVariantAES_KW_128", 9},
      {"kKeyVariantAES_KW_192", 10},
      {"kKeyVariantAES_KW_256", 11},
      {"kKeyVariantAES_OCB_128", 12},
      {"kKeyVariantAES_OCB_192", 13},
      {"kKeyVariantAES_OCB_256", 14},
      {"OPENSSL_EC_EXPLICIT_CURVE", 0},
      {"OPENSSL_EC_NAMED_CURVE", 1},
      {"RSA_PKCS1_PSS_PADDING", 6},
      {"X509_CHECK_FLAG_ALWAYS_CHECK_SUBJECT", 1},
      {"X509_CHECK_FLAG_NO_WILDCARDS", 2},
      {"X509_CHECK_FLAG_NO_PARTIAL_WILDCARDS", 4},
      {"X509_CHECK_FLAG_MULTI_LABEL_WILDCARDS", 8},
      {"X509_CHECK_FLAG_SINGLE_LABEL_SUBDOMAINS", 16},
      {"X509_CHECK_FLAG_NEVER_CHECK_SUBJECT", 32},
      {"EVP_PKEY_ED25519", 1087},
      {"EVP_PKEY_ED448", 1088},
      {"EVP_PKEY_X25519", 1034},
      {"EVP_PKEY_X448", 1035},
      {"EVP_PKEY_ML_DSA_44", 1457},
      {"EVP_PKEY_ML_DSA_65", 1458},
      {"EVP_PKEY_ML_DSA_87", 1459},
      {"EVP_PKEY_ML_KEM_512", 1454},
      {"EVP_PKEY_ML_KEM_768", 1455},
      {"EVP_PKEY_ML_KEM_1024", 1456},
      {"EVP_PKEY_SLH_DSA_SHA2_128S", 1460},
      {"EVP_PKEY_SLH_DSA_SHA2_128F", 1461},
      {"EVP_PKEY_SLH_DSA_SHA2_192S", 1462},
      {"EVP_PKEY_SLH_DSA_SHA2_192F", 1463},
      {"EVP_PKEY_SLH_DSA_SHA2_256S", 1464},
      {"EVP_PKEY_SLH_DSA_SHA2_256F", 1465},
      {"EVP_PKEY_SLH_DSA_SHAKE_128S", 1466},
      {"EVP_PKEY_SLH_DSA_SHAKE_128F", 1467},
      {"EVP_PKEY_SLH_DSA_SHAKE_192S", 1468},
      {"EVP_PKEY_SLH_DSA_SHAKE_192F", 1469},
      {"EVP_PKEY_SLH_DSA_SHAKE_256S", 1470},
      {"EVP_PKEY_SLH_DSA_SHAKE_256F", 1471},
  };
  for (const auto& [name, value] : constants) {
    bool has = false;
    if (napi_has_named_property(env, out, name, &has) == napi_ok && !has) SetNamedInt(env, out, name, value);
  }

  EnsurePropertyMethod(env, out, "oneShotDigest", CryptoOneShotDigest);
  EnsurePropertyMethod(env, out, "timingSafeEqual", CryptoTimingSafeEqual);
  EnsurePropertyMethod(env, out, "secureBuffer", CryptoSecureBuffer);
  EnsurePropertyMethod(env, out, "secureHeapUsed", CryptoSecureHeapUsed);
  EnsurePropertyMethod(env, out, "getCachedAliases", CryptoGetCachedAliases);
  EnsurePropertyMethod(env, out, "getFipsCrypto", CryptoGetFips);
  EnsurePropertyMethod(env, out, "setFipsCrypto", CryptoSetFips);
  EnsurePropertyMethod(env, out, "testFipsCrypto", CryptoTestFips);
  EnsurePropertyMethod(env, out, "getOpenSSLSecLevelCrypto", CryptoGetOpenSSLSecLevel);
  EnsurePropertyMethod(env, out, "getBundledRootCertificates", CryptoGetEmptyArray);
  EnsurePropertyMethod(env, out, "getExtraCACertificates", CryptoGetEmptyArray);
  EnsurePropertyMethod(env, out, "getSystemCACertificates", CryptoGetEmptyArray);
  EnsurePropertyMethod(env, out, "getUserRootCertificates", CryptoGetEmptyArray);
  EnsurePropertyMethod(env, out, "resetRootCertStore", CryptoNoop);
  EnsurePropertyMethod(env, out, "startLoadingCertificatesOffThread", CryptoNoop);
  EnsurePropertyMethod(env, out, "createNativeKeyObjectClass", CryptoCreateNativeKeyObjectClass);
  EnsureStubMethod(env, out, "setEngine");
  EnsureStubMethod(env, out, "privateEncrypt");
  EnsureStubMethod(env, out, "publicDecrypt");
  EnsureStubMethod(env, out, "certExportChallenge");
  EnsureStubMethod(env, out, "certExportPublicKey");
  EnsureStubMethod(env, out, "certVerifySpkac");
  EnsurePropertyMethod(env, out, "parseX509", CryptoParseX509);
  EnsurePropertyMethod(env, out, "getSSLCiphers", CryptoGetSSLCiphers);

  EnsureClass(env,
              out,
              "Hash",
              HashCtor,
              {
                  {"update", nullptr, HashUpdate, nullptr, nullptr, nullptr, napi_default, nullptr},
                  {"digest", nullptr, HashDigest, nullptr, nullptr, nullptr, napi_default, nullptr},
              });

  EnsureClass(env,
              out,
              "Hmac",
              HmacCtor,
              {
                  {"init", nullptr, HmacInit, nullptr, nullptr, nullptr, napi_default, nullptr},
                  {"update", nullptr, HmacUpdate, nullptr, nullptr, nullptr, napi_default, nullptr},
                  {"digest", nullptr, HmacDigest, nullptr, nullptr, nullptr, napi_default, nullptr},
              });

  // Placeholder classes for parity surface.
  EnsureClass(env, out, "Sign", HmacCtor, {});
  EnsureClass(env, out, "Verify", HmacCtor, {});
  EnsureClass(env, out, "CipherBase", HmacCtor, {});

  EnsureClass(env,
              out,
              "SecureContext",
              SecureContextCtor,
              {
                  {"init", nullptr, SecureContextInit, nullptr, nullptr, nullptr, napi_default, nullptr},
                  {"setOptions", nullptr, SecureContextSetOptions, nullptr, nullptr, nullptr, napi_default, nullptr},
                  {"getMinProto", nullptr, SecureContextGetMinProto, nullptr, nullptr, nullptr, napi_default, nullptr},
                  {"getMaxProto", nullptr, SecureContextGetMaxProto, nullptr, nullptr, nullptr, napi_default, nullptr},
                  {"close", nullptr, SecureContextClose, nullptr, nullptr, nullptr, napi_default, nullptr},
              });

  EnsureClass(env,
              out,
              "KeyObjectHandle",
              KeyObjectCtor,
              {
                  {"init", nullptr, KeyObjectInit, nullptr, nullptr, nullptr, napi_default, nullptr},
                  {"export", nullptr, KeyObjectExport, nullptr, nullptr, nullptr, napi_default, nullptr},
                  {"getSymmetricKeySize", nullptr, KeyObjectGetSymmetricKeySize, nullptr, nullptr, nullptr, napi_default,
                   nullptr},
                  {"equals", nullptr, KeyObjectEquals, nullptr, nullptr, nullptr, napi_default, nullptr},
              });

  EnsureClass(env,
              out,
              "RandomBytesJob",
              JobCtor,
              {
                  {"run", nullptr, RandomBytesJobRun, nullptr, nullptr, nullptr, napi_default, nullptr},
              });
  EnsureClass(env,
              out,
              "PBKDF2Job",
              JobCtor,
              {
                  {"run", nullptr, PBKDF2JobRun, nullptr, nullptr, nullptr, napi_default, nullptr},
              });
  EnsureClass(env,
              out,
              "ScryptJob",
              JobCtor,
              {
                  {"run", nullptr, ScryptJobRun, nullptr, nullptr, nullptr, napi_default, nullptr},
              });
  EnsureClass(env,
              out,
              "HKDFJob",
              JobCtor,
              {
                  {"run", nullptr, HKDFJobRun, nullptr, nullptr, nullptr, napi_default, nullptr},
              });
  EnsureClass(env,
              out,
              "HashJob",
              JobCtor,
              {
                  {"run", nullptr, HashJobRun, nullptr, nullptr, nullptr, napi_default, nullptr},
              });
  EnsureClass(env,
              out,
              "SignJob",
              JobCtor,
              {
                  {"run", nullptr, SignJobRun, nullptr, nullptr, nullptr, napi_default, nullptr},
              });

  const char* stub_classes[] = {
      "AESCipherJob",      "Argon2Job",          "ChaCha20Poly1305CipherJob", "CheckPrimeJob",
      "DHBitsJob",         "DHKeyExportJob",     "DhKeyPairGenJob",           "DiffieHellman",
      "DiffieHellmanGroup","DsaKeyPairGenJob",   "ECDH",                      "ECDHBitsJob",
      "ECDHConvertKey",    "ECKeyExportJob",     "EcKeyPairGenJob",           "HmacJob",
      "KEMDecapsulateJob", "KEMEncapsulateJob",  "KmacJob",                   "NidKeyPairGenJob",
      "RSACipherJob",      "RSAKeyExportJob",    "RandomPrimeJob",            "RsaKeyPairGenJob",
      "SecretKeyGenJob",
  };
  for (const char* cls : stub_classes) EnsureStubClass(env, out, cls);

  return out;
}

}  // namespace internal_binding
