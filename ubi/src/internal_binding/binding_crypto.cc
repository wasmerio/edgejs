#include "internal_binding/dispatch.h"

#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <openssl/bio.h>
#include <openssl/bn.h>
#include <openssl/crypto.h>
#include <openssl/core_names.h>
#include <openssl/dh.h>
#include <openssl/ec.h>
#include <openssl/ecdh.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/obj_mac.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>
#include <openssl/x509.h>

#include "ncrypto.h"
#include "internal_binding/helpers.h"
#include "ubi_async_wrap.h"
#include "ubi_runtime.h"
#include "ubi_runtime_platform.h"

namespace internal_binding {

namespace {

constexpr int32_t kCryptoJobAsync = 0;
constexpr int32_t kCryptoJobSync = 1;
constexpr int32_t kSignJobModeSign = 0;
constexpr int32_t kSignJobModeVerify = 1;
constexpr int32_t kKeyTypeSecret = 0;
constexpr int32_t kKeyTypePublic = 1;
constexpr int32_t kKeyTypePrivate = 2;
constexpr int32_t kKeyFormatDER = 0;
constexpr int32_t kKeyFormatPEM = 1;
constexpr int32_t kKeyFormatJWK = 2;
constexpr int32_t kKeyEncodingPKCS1 = 0;
constexpr int32_t kKeyEncodingPKCS8 = 1;
constexpr int32_t kKeyEncodingSPKI = 2;
constexpr int32_t kKeyEncodingSEC1 = 3;
constexpr int32_t kKeyVariantRSA_SSA_PKCS1_v1_5 = 0;
constexpr int32_t kKeyVariantRSA_PSS = 1;

struct CryptoBindingState {
  napi_ref binding_ref = nullptr;
  int32_t fips_mode = 0;
};

std::unordered_map<napi_env, CryptoBindingState> g_crypto_states;
std::unordered_set<napi_env> g_crypto_cleanup_hooks;

void ResetRef(napi_env env, napi_ref* ref_ptr);

void OnCryptoEnvCleanup(void* data) {
  napi_env env = static_cast<napi_env>(data);
  g_crypto_cleanup_hooks.erase(env);
  auto it = g_crypto_states.find(env);
  if (it == g_crypto_states.end()) return;
  ResetRef(env, &it->second.binding_ref);
  g_crypto_states.erase(it);
}

void EnsureCryptoCleanupHook(napi_env env) {
  if (env == nullptr) return;
  auto [it, inserted] = g_crypto_cleanup_hooks.emplace(env);
  if (!inserted) return;
  if (napi_add_env_cleanup_hook(env, OnCryptoEnvCleanup, env) != napi_ok) {
    g_crypto_cleanup_hooks.erase(it);
  }
}

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
    size_t bytes_per_element = 1;
    switch (type) {
      case napi_int8_array:
      case napi_uint8_array:
      case napi_uint8_clamped_array:
        bytes_per_element = 1;
        break;
      case napi_int16_array:
      case napi_uint16_array:
        bytes_per_element = 2;
        break;
      case napi_int32_array:
      case napi_uint32_array:
      case napi_float32_array:
        bytes_per_element = 4;
        break;
      case napi_float64_array:
      case napi_bigint64_array:
      case napi_biguint64_array:
        bytes_per_element = 8;
        break;
      default:
        bytes_per_element = 1;
        break;
    }
    *data = static_cast<const uint8_t*>(raw);
    *length = len * bytes_per_element;
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

std::vector<uint8_t> ValueToBytesWithEncoding(napi_env env, napi_value value, napi_value encoding) {
  if (value == nullptr) return {};

  napi_valuetype value_type = napi_undefined;
  if (napi_typeof(env, value, &value_type) == napi_ok &&
      value_type == napi_string &&
      encoding != nullptr &&
      !IsUndefined(env, encoding)) {
    napi_valuetype encoding_type = napi_undefined;
    if (napi_typeof(env, encoding, &encoding_type) == napi_ok && encoding_type == napi_string) {
      napi_value global = GetGlobal(env);
      napi_value buffer_ctor = nullptr;
      napi_value from_fn = nullptr;
      napi_valuetype from_type = napi_undefined;
      if (global != nullptr &&
          napi_get_named_property(env, global, "Buffer", &buffer_ctor) == napi_ok &&
          buffer_ctor != nullptr &&
          napi_get_named_property(env, buffer_ctor, "from", &from_fn) == napi_ok &&
          from_fn != nullptr &&
          napi_typeof(env, from_fn, &from_type) == napi_ok &&
          from_type == napi_function) {
        napi_value argv[2] = {value, encoding};
        napi_value out = nullptr;
        if (napi_call_function(env, buffer_ctor, from_fn, 2, argv, &out) == napi_ok && out != nullptr) {
          return ValueToBytes(env, out);
        }
      }
    }
  }

  return ValueToBytes(env, value);
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

  napi_value encoding_string = nullptr;
  if (napi_coerce_to_string(env, encoding_value, &encoding_string) != napi_ok || encoding_string == nullptr) {
    return nullptr;
  }
  size_t len = 0;
  if (napi_get_value_string_utf8(env, encoding_string, nullptr, 0, &len) != napi_ok) return buffer_value;
  std::string encoding(len + 1, '\0');
  size_t copied = 0;
  if (napi_get_value_string_utf8(env, encoding_string, encoding.data(), encoding.size(), &copied) != napi_ok) {
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

bool IsNullOrUndefinedValue(napi_env env, napi_value value) {
  if (value == nullptr) return true;
  napi_valuetype type = napi_undefined;
  if (napi_typeof(env, value, &type) != napi_ok) return true;
  return type == napi_undefined || type == napi_null;
}

std::string GetStringValue(napi_env env, napi_value value) {
  if (value == nullptr) return "";
  size_t len = 0;
  if (napi_get_value_string_utf8(env, value, nullptr, 0, &len) != napi_ok) return "";
  std::string out(len + 1, '\0');
  size_t copied = 0;
  if (napi_get_value_string_utf8(env, value, out.data(), out.size(), &copied) != napi_ok) return "";
  out.resize(copied);
  return out;
}

int32_t GetInt32Value(napi_env env, napi_value value, int32_t fallback = 0) {
  if (value == nullptr) return fallback;
  int32_t out = fallback;
  (void)napi_get_value_int32(env, value, &out);
  return out;
}

bool GetNamedInt32Value(napi_env env, napi_value value, const char* key, int32_t* out) {
  if (value == nullptr || key == nullptr || out == nullptr) return false;
  napi_value prop = nullptr;
  if (napi_get_named_property(env, value, key, &prop) != napi_ok || prop == nullptr) return false;
  napi_valuetype type = napi_undefined;
  if (napi_typeof(env, prop, &type) != napi_ok || type != napi_number) return false;
  return napi_get_value_int32(env, prop, out) == napi_ok;
}

bool GetNamedStringValue(napi_env env, napi_value value, const char* key, std::string* out) {
  if (value == nullptr || key == nullptr || out == nullptr) return false;
  napi_value prop = nullptr;
  if (napi_get_named_property(env, value, key, &prop) != napi_ok || prop == nullptr) return false;
  size_t len = 0;
  if (napi_get_value_string_utf8(env, prop, nullptr, 0, &len) != napi_ok) return false;
  std::string text(len + 1, '\0');
  size_t copied = 0;
  if (napi_get_value_string_utf8(env, prop, text.data(), text.size(), &copied) != napi_ok) return false;
  text.resize(copied);
  *out = std::move(text);
  return true;
}

napi_value CopyAsArrayBuffer(napi_env env, napi_value value) {
  if (value == nullptr || IsUndefined(env, value)) return value;
  const uint8_t* data = nullptr;
  size_t len = 0;
  if (!GetByteSpan(env, value, &data, &len)) return value;
  napi_value out = nullptr;
  void* raw = nullptr;
  if (napi_create_arraybuffer(env, len, &raw, &out) != napi_ok || out == nullptr || raw == nullptr) {
    return value;
  }
  if (len > 0) std::memcpy(raw, data, len);
  return out;
}

void SetClassPrototypeMethod(napi_env env,
                             napi_value binding,
                             const char* class_name,
                             const char* method_name,
                             napi_callback cb) {
  if (binding == nullptr || class_name == nullptr || method_name == nullptr || cb == nullptr) return;
  napi_value ctor = nullptr;
  if (napi_get_named_property(env, binding, class_name, &ctor) != napi_ok || ctor == nullptr) return;
  napi_valuetype ctor_type = napi_undefined;
  if (napi_typeof(env, ctor, &ctor_type) != napi_ok || ctor_type != napi_function) return;
  napi_value proto = nullptr;
  if (napi_get_named_property(env, ctor, "prototype", &proto) != napi_ok || proto == nullptr) return;
  napi_value fn = nullptr;
  if (napi_create_function(env, method_name, NAPI_AUTO_LENGTH, cb, nullptr, &fn) != napi_ok || fn == nullptr) return;
  napi_set_named_property(env, proto, method_name, fn);
}

bool IsSupportedDigestName(napi_env env, napi_value binding, napi_value digest) {
  if (binding == nullptr || digest == nullptr || IsUndefined(env, digest) || IsNullOrUndefinedValue(env, digest)) {
    return false;
  }
  const napi_value empty = BytesToBuffer(env, {});
  napi_value argv[2] = {digest, empty != nullptr ? empty : Undefined(env)};
  napi_value out = nullptr;
  if (CallBindingMethod(env, binding, "hashOneShot", 2, argv, &out)) return true;
  bool pending = false;
  if (napi_is_exception_pending(env, &pending) == napi_ok && pending) {
    napi_value ignored = nullptr;
    napi_get_and_clear_last_exception(env, &ignored);
  }
  return false;
}

napi_value CreateErrorWithCode(napi_env env, const char* code, const std::string& message) {
  napi_value code_v = nullptr;
  napi_value msg_v = nullptr;
  napi_value err_v = nullptr;
  napi_create_string_utf8(env, code, NAPI_AUTO_LENGTH, &code_v);
  napi_create_string_utf8(env, message.c_str(), NAPI_AUTO_LENGTH, &msg_v);
  bool as_type_error = false;
  bool as_range_error = false;
  if (code != nullptr) {
    if (std::strcmp(code, "ERR_INVALID_ARG_TYPE") == 0 ||
        std::strcmp(code, "ERR_INVALID_ARG_VALUE") == 0 ||
        std::strcmp(code, "ERR_MISSING_OPTION") == 0 ||
        std::strcmp(code, "ERR_CRYPTO_INVALID_CURVE") == 0) {
      as_type_error = true;
    } else if (std::strcmp(code, "ERR_OUT_OF_RANGE") == 0 ||
               std::strcmp(code, "ERR_CRYPTO_INVALID_KEYLEN") == 0) {
      as_range_error = true;
    }
  }
  if (as_type_error) {
    napi_create_type_error(env, code_v, msg_v, &err_v);
  } else if (as_range_error) {
    napi_create_range_error(env, code_v, msg_v, &err_v);
  } else {
    napi_create_error(env, code_v, msg_v, &err_v);
  }
  if (err_v != nullptr && code_v != nullptr) napi_set_named_property(env, err_v, "code", code_v);
  return err_v != nullptr ? err_v : Undefined(env);
}

napi_value BuildJobResult(napi_env env, napi_value err, napi_value value);

napi_value BuildJobErrorResult(napi_env env, const char* code, const std::string& message) {
  return BuildJobResult(env, CreateErrorWithCode(env, code, message), Undefined(env));
}

std::vector<uint8_t> BytesFromBio(BIO* bio) {
  std::vector<uint8_t> out;
  if (bio == nullptr) return out;
  const char* data = nullptr;
  const long len = BIO_get_mem_data(bio, &data);
  if (len <= 0 || data == nullptr) return out;
  out.assign(reinterpret_cast<const uint8_t*>(data), reinterpret_cast<const uint8_t*>(data) + len);
  return out;
}

std::vector<uint8_t> BigNumToBytes(const BIGNUM* bn) {
  std::vector<uint8_t> out;
  if (bn == nullptr) return out;
  const int len = BN_num_bytes(bn);
  if (len <= 0) return out;
  out.resize(static_cast<size_t>(len));
  BN_bn2bin(bn, out.data());
  return out;
}

std::string Base64UrlEncode(const std::vector<uint8_t>& in) {
  if (in.empty()) return "";
  const int encoded_len = 4 * static_cast<int>((in.size() + 2) / 3);
  std::string out(static_cast<size_t>(encoded_len), '\0');
  const int written = EVP_EncodeBlock(reinterpret_cast<unsigned char*>(out.data()),
                                      reinterpret_cast<const unsigned char*>(in.data()),
                                      static_cast<int>(in.size()));
  if (written <= 0) return "";
  out.resize(static_cast<size_t>(written));
  for (char& ch : out) {
    if (ch == '+') {
      ch = '-';
    } else if (ch == '/') {
      ch = '_';
    }
  }
  while (!out.empty() && out.back() == '=') out.pop_back();
  return out;
}

void SetObjectString(napi_env env, napi_value obj, const char* name, const std::string& value) {
  napi_value v = nullptr;
  if (napi_create_string_utf8(env, value.c_str(), NAPI_AUTO_LENGTH, &v) == napi_ok && v != nullptr) {
    napi_set_named_property(env, obj, name, v);
  }
}

void SetObjectBuffer(napi_env env, napi_value obj, const char* name, const std::vector<uint8_t>& value) {
  napi_value v = BytesToBuffer(env, value);
  if (v != nullptr && !IsUndefined(env, v)) napi_set_named_property(env, obj, name, v);
}

void SetJwkFieldFromBigNum(napi_env env, napi_value obj, const char* name, const BIGNUM* bn) {
  if (obj == nullptr || name == nullptr || bn == nullptr) return;
  const std::string encoded = Base64UrlEncode(BigNumToBytes(bn));
  SetObjectString(env, obj, name, encoded);
}

int CurveNidFromName(const std::string& name) {
  if (name.empty()) return NID_undef;
  if (name == "P-256") return NID_X9_62_prime256v1;
  if (name == "P-384") return NID_secp384r1;
  if (name == "P-521") return NID_secp521r1;
  int nid = OBJ_txt2nid(name.c_str());
  if (nid == NID_undef) nid = OBJ_sn2nid(name.c_str());
  if (nid == NID_undef) nid = OBJ_ln2nid(name.c_str());
  return nid;
}

std::string JwkCurveFromNid(int nid) {
  switch (nid) {
    case NID_X9_62_prime256v1:
      return "P-256";
    case NID_secp384r1:
      return "P-384";
    case NID_secp521r1:
      return "P-521";
    case NID_secp256k1:
      return "secp256k1";
    default:
      return "";
  }
}

std::string AsymmetricKeyTypeName(const EVP_PKEY* pkey) {
  if (pkey == nullptr) return "";
  switch (EVP_PKEY_base_id(pkey)) {
    case EVP_PKEY_RSA:
      return "rsa";
    case EVP_PKEY_RSA_PSS:
      return "rsa-pss";
    case EVP_PKEY_DSA:
      return "dsa";
    case EVP_PKEY_DH:
      return "dh";
    case EVP_PKEY_EC:
      return "ec";
    case EVP_PKEY_ED25519:
      return "ed25519";
    case EVP_PKEY_ED448:
      return "ed448";
    case EVP_PKEY_X25519:
      return "x25519";
    case EVP_PKEY_X448:
      return "x448";
    default:
      return "";
  }
}

EVP_PKEY* ParsePrivateKeyBytes(const uint8_t* data, size_t len) {
  if (data == nullptr || len == 0) return nullptr;
  BIO* bio = BIO_new_mem_buf(data, static_cast<int>(len));
  if (bio == nullptr) return nullptr;
  EVP_PKEY* pkey = PEM_read_bio_PrivateKey(bio, nullptr, nullptr, nullptr);
  if (pkey == nullptr) {
    (void)BIO_reset(bio);
    pkey = d2i_PrivateKey_bio(bio, nullptr);
  }
  BIO_free(bio);
  return pkey;
}

EVP_PKEY* ParsePublicKeyBytes(const uint8_t* data, size_t len) {
  if (data == nullptr || len == 0) return nullptr;
  BIO* bio = BIO_new_mem_buf(data, static_cast<int>(len));
  if (bio == nullptr) return nullptr;
  EVP_PKEY* pkey = PEM_read_bio_PUBKEY(bio, nullptr, nullptr, nullptr);
  if (pkey == nullptr) {
    (void)BIO_reset(bio);
    pkey = d2i_PUBKEY_bio(bio, nullptr);
  }
  if (pkey == nullptr) {
    (void)BIO_reset(bio);
    X509* cert = PEM_read_bio_X509(bio, nullptr, nullptr, nullptr);
    if (cert == nullptr) {
      (void)BIO_reset(bio);
      cert = d2i_X509_bio(bio, nullptr);
    }
    if (cert != nullptr) {
      pkey = X509_get_pubkey(cert);
      X509_free(cert);
    }
  }
  BIO_free(bio);
  return pkey;
}

EVP_PKEY* ParseAnyKeyBytes(const std::vector<uint8_t>& data) {
  EVP_PKEY* pkey = ParsePrivateKeyBytes(data.data(), data.size());
  if (pkey == nullptr) pkey = ParsePublicKeyBytes(data.data(), data.size());
  return pkey;
}

struct HashWrap {
  napi_ref wrapper_ref = nullptr;
  std::string algorithm;
  std::vector<uint8_t> data;
  int32_t xof_len = -1;
  bool finalized = false;
  std::vector<uint8_t> digest_cache;
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
  size_t argc = 4;
  napi_value argv[4] = {nullptr, nullptr, nullptr, nullptr};
  napi_value this_arg = nullptr;
  if (napi_get_cb_info(env, info, &argc, argv, &this_arg, nullptr) != napi_ok || this_arg == nullptr) {
    return nullptr;
  }
  auto* wrap = new HashWrap();
  if (argc >= 1 && argv[0] != nullptr) {
    HashWrap* source = UnwrapHash(env, argv[0]);
    if (source != nullptr) {
      wrap->algorithm = source->algorithm;
      wrap->data = source->data;
      wrap->xof_len = source->xof_len;
      wrap->finalized = source->finalized;
      wrap->digest_cache = source->digest_cache;
      napi_wrap(env, this_arg, wrap, HashFinalize, nullptr, &wrap->wrapper_ref);
      return this_arg;
    }
  }
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
  if (argc >= 2 && argv[1] != nullptr) {
    napi_get_value_int32(env, argv[1], &wrap->xof_len);
  }
  if (wrap->algorithm.empty()) wrap->algorithm = "sha256";
  napi_wrap(env, this_arg, wrap, HashFinalize, nullptr, &wrap->wrapper_ref);
  return this_arg;
}

napi_value HashUpdate(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value argv[2] = {nullptr, nullptr};
  napi_value this_arg = nullptr;
  napi_get_cb_info(env, info, &argc, argv, &this_arg, nullptr);
  HashWrap* wrap = UnwrapHash(env, this_arg);
  if (wrap == nullptr) return this_arg != nullptr ? this_arg : Undefined(env);
  if (wrap->finalized) return this_arg != nullptr ? this_arg : Undefined(env);
  if (argc >= 1 && argv[0] != nullptr) {
    std::vector<uint8_t> input = ValueToBytesWithEncoding(env, argv[0], argc >= 2 ? argv[1] : nullptr);
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

  if (wrap->finalized) {
    napi_value cached = BytesToBuffer(env, wrap->digest_cache);
    return MaybeToEncodedOutput(env, cached, argc >= 1 ? argv[0] : nullptr);
  }
  napi_value binding = GetBinding(env);
  if (binding == nullptr) return Undefined(env);

  napi_value algorithm = nullptr;
  napi_create_string_utf8(env, wrap->algorithm.c_str(), NAPI_AUTO_LENGTH, &algorithm);
  napi_value data = BytesToBuffer(env, wrap->data);
  napi_value out = nullptr;
  if (wrap->xof_len >= 0) {
    napi_value xof_len_value = nullptr;
    napi_create_int32(env, wrap->xof_len, &xof_len_value);
    napi_value call_argv[3] = {algorithm, data, xof_len_value};
    if (!CallBindingMethod(env, binding, "hashOneShotXof", 3, call_argv, &out)) return Undefined(env);
  } else {
    napi_value call_argv[2] = {algorithm, data};
    if (!CallBindingMethod(env, binding, "hashOneShot", 2, call_argv, &out)) return Undefined(env);
  }
  napi_value as_buffer = EnsureBufferValue(env, out);
  wrap->digest_cache = ValueToBytes(env, as_buffer);
  wrap->finalized = true;
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

  // Match Node behavior: invalid digest names throw during createHmac/init.
  napi_value binding = GetBinding(env);
  if (binding != nullptr) {
    napi_value algorithm = nullptr;
    napi_create_string_utf8(env, wrap->algorithm.c_str(), NAPI_AUTO_LENGTH, &algorithm);
    napi_value key = BytesToBuffer(env, wrap->key);
    napi_value empty = BytesToBuffer(env, {});
    napi_value validate_argv[3] = {algorithm != nullptr ? algorithm : Undefined(env),
                                   key != nullptr ? key : Undefined(env),
                                   empty != nullptr ? empty : Undefined(env)};
    napi_value ignored = nullptr;
    if (!CallBindingMethod(env, binding, "hmacOneShot", 3, validate_argv, &ignored)) {
      return nullptr;
    }
  }
  return Undefined(env);
}

napi_value HmacUpdate(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value argv[2] = {nullptr, nullptr};
  napi_value this_arg = nullptr;
  napi_get_cb_info(env, info, &argc, argv, &this_arg, nullptr);
  HmacWrap* wrap = UnwrapHmac(env, this_arg);
  if (wrap == nullptr) return this_arg != nullptr ? this_arg : Undefined(env);
  if (argc >= 1 && argv[0] != nullptr) {
    std::vector<uint8_t> bytes = ValueToBytesWithEncoding(env, argv[0], argc >= 2 ? argv[1] : nullptr);
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

struct SignVerifyWrap {
  napi_ref wrapper_ref = nullptr;
  std::string algorithm;
  std::vector<uint8_t> data;
  bool verify = false;
};

void SignVerifyFinalize(napi_env env, void* data, void* /*hint*/) {
  auto* wrap = static_cast<SignVerifyWrap*>(data);
  if (wrap == nullptr) return;
  ResetRef(env, &wrap->wrapper_ref);
  delete wrap;
}

SignVerifyWrap* UnwrapSignVerify(napi_env env, napi_value this_arg) {
  if (this_arg == nullptr) return nullptr;
  void* data = nullptr;
  if (napi_unwrap(env, this_arg, &data) != napi_ok || data == nullptr) return nullptr;
  return static_cast<SignVerifyWrap*>(data);
}

napi_value SignCtor(napi_env env, napi_callback_info info) {
  napi_value this_arg = nullptr;
  size_t argc = 0;
  napi_get_cb_info(env, info, &argc, nullptr, &this_arg, nullptr);
  if (this_arg == nullptr) return nullptr;
  auto* wrap = new SignVerifyWrap();
  wrap->algorithm = "sha256";
  wrap->verify = false;
  napi_wrap(env, this_arg, wrap, SignVerifyFinalize, nullptr, &wrap->wrapper_ref);
  return this_arg;
}

napi_value VerifyCtor(napi_env env, napi_callback_info info) {
  napi_value this_arg = nullptr;
  size_t argc = 0;
  napi_get_cb_info(env, info, &argc, nullptr, &this_arg, nullptr);
  if (this_arg == nullptr) return nullptr;
  auto* wrap = new SignVerifyWrap();
  wrap->algorithm = "sha256";
  wrap->verify = true;
  napi_wrap(env, this_arg, wrap, SignVerifyFinalize, nullptr, &wrap->wrapper_ref);
  return this_arg;
}

napi_value SignVerifyInit(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  napi_value this_arg = nullptr;
  napi_get_cb_info(env, info, &argc, argv, &this_arg, nullptr);
  SignVerifyWrap* wrap = UnwrapSignVerify(env, this_arg);
  if (wrap == nullptr) return Undefined(env);
  if (argc >= 1 && argv[0] != nullptr) {
    size_t len = 0;
    if (napi_get_value_string_utf8(env, argv[0], nullptr, 0, &len) == napi_ok) {
      wrap->algorithm.resize(len + 1);
      size_t copied = 0;
      if (napi_get_value_string_utf8(env, argv[0], wrap->algorithm.data(), wrap->algorithm.size(), &copied) ==
          napi_ok) {
        wrap->algorithm.resize(copied);
      }
    }
  }
  wrap->data.clear();

  if (!wrap->algorithm.empty()) {
    napi_value binding = GetBinding(env);
    napi_value algorithm = nullptr;
    napi_create_string_utf8(env, wrap->algorithm.c_str(), NAPI_AUTO_LENGTH, &algorithm);
    if (binding != nullptr && algorithm != nullptr && !IsSupportedDigestName(env, binding, algorithm)) {
      napi_throw(env, CreateErrorWithCode(env, "ERR_CRYPTO_INVALID_DIGEST", "Invalid digest"));
      return nullptr;
    }
  }
  return Undefined(env);
}

napi_value SignVerifyUpdate(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value argv[2] = {nullptr, nullptr};
  napi_value this_arg = nullptr;
  napi_get_cb_info(env, info, &argc, argv, &this_arg, nullptr);
  SignVerifyWrap* wrap = UnwrapSignVerify(env, this_arg);
  if (wrap == nullptr) return Undefined(env);
  if (argc >= 1 && argv[0] != nullptr) {
    std::vector<uint8_t> bytes = ValueToBytesWithEncoding(env, argv[0], argc >= 2 ? argv[1] : nullptr);
    wrap->data.insert(wrap->data.end(), bytes.begin(), bytes.end());
  }
  return this_arg != nullptr ? this_arg : Undefined(env);
}

napi_value SignSign(napi_env env, napi_callback_info info) {
  size_t argc = 7;
  napi_value argv[7] = {nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr};
  napi_value this_arg = nullptr;
  napi_get_cb_info(env, info, &argc, argv, &this_arg, nullptr);
  SignVerifyWrap* wrap = UnwrapSignVerify(env, this_arg);
  if (wrap == nullptr) return Undefined(env);
  napi_value binding = GetBinding(env);
  if (binding == nullptr) return Undefined(env);

  napi_value algorithm = nullptr;
  napi_create_string_utf8(env, wrap->algorithm.c_str(), NAPI_AUTO_LENGTH, &algorithm);
  if (algorithm == nullptr) napi_get_null(env, &algorithm);

  napi_value data = BytesToBuffer(env, wrap->data);
  napi_value key_data = argc >= 1 ? argv[0] : Undefined(env);
  napi_value key_format = argc >= 2 ? argv[1] : Undefined(env);
  napi_value key_type = argc >= 3 ? argv[2] : Undefined(env);
  napi_value key_passphrase = argc >= 4 ? argv[3] : Undefined(env);
  napi_value padding = argc >= 5 ? argv[4] : Undefined(env);
  napi_value salt = argc >= 6 ? argv[5] : Undefined(env);
  napi_value dsa_sig_enc = argc >= 7 ? argv[6] : Undefined(env);
  napi_value context = Undefined(env);
  napi_value call_argv[10] = {
      algorithm,
      data,
      key_data,
      key_format,
      key_type,
      key_passphrase,
      padding,
      salt,
      dsa_sig_enc,
      context,
  };
  napi_value out = nullptr;
  if (!CallBindingMethod(env, binding, "signOneShot", 10, call_argv, &out)) return nullptr;
  wrap->data.clear();
  return EnsureBufferValue(env, out);
}

napi_value VerifyVerify(napi_env env, napi_callback_info info) {
  size_t argc = 8;
  napi_value argv[8] = {nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr};
  napi_value this_arg = nullptr;
  napi_get_cb_info(env, info, &argc, argv, &this_arg, nullptr);
  SignVerifyWrap* wrap = UnwrapSignVerify(env, this_arg);
  if (wrap == nullptr) return Undefined(env);
  napi_value binding = GetBinding(env);
  if (binding == nullptr) return Undefined(env);

  napi_value algorithm = nullptr;
  napi_create_string_utf8(env, wrap->algorithm.c_str(), NAPI_AUTO_LENGTH, &algorithm);
  if (algorithm == nullptr) napi_get_null(env, &algorithm);

  napi_value data = BytesToBuffer(env, wrap->data);
  napi_value key_data = argc >= 1 ? argv[0] : Undefined(env);
  napi_value key_format = argc >= 2 ? argv[1] : Undefined(env);
  napi_value key_type = argc >= 3 ? argv[2] : Undefined(env);
  napi_value key_passphrase = argc >= 4 ? argv[3] : Undefined(env);
  napi_value signature = EnsureBufferValue(env, argc >= 5 ? argv[4] : Undefined(env));
  napi_value padding = argc >= 6 ? argv[5] : Undefined(env);
  napi_value salt = argc >= 7 ? argv[6] : Undefined(env);
  napi_value dsa_sig_enc = argc >= 8 ? argv[7] : Undefined(env);
  napi_value context = Undefined(env);
  napi_value call_argv[11] = {
      algorithm,
      data,
      key_data,
      key_format,
      key_type,
      key_passphrase,
      signature,
      padding,
      salt,
      dsa_sig_enc,
      context,
  };
  napi_value out = nullptr;
  if (!CallBindingMethod(env, binding, "verifyOneShot", 11, call_argv, &out)) return nullptr;
  wrap->data.clear();
  return out != nullptr ? out : Undefined(env);
}

struct DiffieHellmanWrap {
  napi_ref wrapper_ref = nullptr;
  DH* dh = nullptr;
  int32_t verify_error = 0;
};

void DiffieHellmanFinalize(napi_env env, void* data, void* /*hint*/) {
  auto* wrap = static_cast<DiffieHellmanWrap*>(data);
  if (wrap == nullptr) return;
  if (wrap->dh != nullptr) {
    DH_free(wrap->dh);
    wrap->dh = nullptr;
  }
  ResetRef(env, &wrap->wrapper_ref);
  delete wrap;
}

DiffieHellmanWrap* UnwrapDiffieHellman(napi_env env, napi_value this_arg) {
  if (this_arg == nullptr) return nullptr;
  void* data = nullptr;
  if (napi_unwrap(env, this_arg, &data) != napi_ok || data == nullptr) return nullptr;
  return static_cast<DiffieHellmanWrap*>(data);
}

DH* CreateDhFromPQG(BIGNUM* prime, BIGNUM* generator, int32_t* verify_error) {
  if (prime == nullptr || generator == nullptr) {
    if (prime != nullptr) BN_free(prime);
    if (generator != nullptr) BN_free(generator);
    return nullptr;
  }
  DH* dh = DH_new();
  if (dh == nullptr) {
    BN_free(prime);
    BN_free(generator);
    return nullptr;
  }
  if (DH_set0_pqg(dh, prime, nullptr, generator) != 1) {
    BN_free(prime);
    BN_free(generator);
    DH_free(dh);
    return nullptr;
  }
  int codes = 0;
  if (DH_check(dh, &codes) == 1 && verify_error != nullptr) *verify_error = codes;
  return dh;
}

BIGNUM* DhPrimeFromBitLength(int32_t bits) {
  switch (bits) {
    case 768:
      return BN_get_rfc2409_prime_768(nullptr);
    case 1024:
      return BN_get_rfc2409_prime_1024(nullptr);
    case 1536:
      return BN_get_rfc3526_prime_1536(nullptr);
    case 2048:
      return BN_get_rfc3526_prime_2048(nullptr);
    case 3072:
      return BN_get_rfc3526_prime_3072(nullptr);
    case 4096:
      return BN_get_rfc3526_prime_4096(nullptr);
    case 6144:
      return BN_get_rfc3526_prime_6144(nullptr);
    case 8192:
      return BN_get_rfc3526_prime_8192(nullptr);
    default:
      return nullptr;
  }
}

BIGNUM* DhPrimeFromGroupName(const std::string& name) {
  if (name == "modp1") return BN_get_rfc2409_prime_768(nullptr);
  if (name == "modp2") return BN_get_rfc2409_prime_1024(nullptr);
  if (name == "modp5") return BN_get_rfc3526_prime_1536(nullptr);
  if (name == "modp14") return BN_get_rfc3526_prime_2048(nullptr);
  if (name == "modp15") return BN_get_rfc3526_prime_3072(nullptr);
  if (name == "modp16") return BN_get_rfc3526_prime_4096(nullptr);
  if (name == "modp17") return BN_get_rfc3526_prime_6144(nullptr);
  if (name == "modp18") return BN_get_rfc3526_prime_8192(nullptr);
  return nullptr;
}

BIGNUM* DhGeneratorFromNumber(int32_t generator_number) {
  if (generator_number == 0) generator_number = 2;
  BIGNUM* g = BN_new();
  if (g == nullptr) return nullptr;
  if (BN_set_word(g, static_cast<BN_ULONG>(generator_number)) != 1) {
    BN_free(g);
    return nullptr;
  }
  return g;
}

DH* CreateDhFromSize(int32_t bits,
                     int32_t generator_number,
                     const std::vector<uint8_t>& generator_bytes,
                     int32_t* verify_error) {
  if (bits <= 0) return nullptr;

  BIGNUM* prime = DhPrimeFromBitLength(bits);
  if (prime != nullptr) {
    BIGNUM* generator = nullptr;
    if (!generator_bytes.empty()) {
      generator = BN_bin2bn(generator_bytes.data(), static_cast<int>(generator_bytes.size()), nullptr);
    } else {
      generator = DhGeneratorFromNumber(generator_number);
    }
    return CreateDhFromPQG(prime, generator, verify_error);
  }

  if (!generator_bytes.empty()) {
    return nullptr;
  }
  if (generator_number == 0) generator_number = 2;
  DH* dh = DH_new();
  if (dh == nullptr) return nullptr;
  if (DH_generate_parameters_ex(dh, bits, generator_number, nullptr) != 1) {
    DH_free(dh);
    return nullptr;
  }
  int codes = 0;
  if (DH_check(dh, &codes) == 1 && verify_error != nullptr) *verify_error = codes;
  return dh;
}

DH* CreateDhFromPrimeAndGenerator(const std::vector<uint8_t>& prime_bytes,
                                  int32_t generator_number,
                                  const std::vector<uint8_t>& generator_bytes,
                                  int32_t* verify_error) {
  if (prime_bytes.empty()) return nullptr;
  BIGNUM* prime = BN_bin2bn(prime_bytes.data(), static_cast<int>(prime_bytes.size()), nullptr);
  BIGNUM* generator = nullptr;
  if (!generator_bytes.empty()) {
    generator = BN_bin2bn(generator_bytes.data(), static_cast<int>(generator_bytes.size()), nullptr);
  } else {
    generator = DhGeneratorFromNumber(generator_number);
  }
  return CreateDhFromPQG(prime, generator, verify_error);
}

DH* CreateDhFromGroupName(const std::string& group_name, int32_t* verify_error) {
  BIGNUM* prime = DhPrimeFromGroupName(group_name);
  if (prime == nullptr) return nullptr;
  BIGNUM* generator = DhGeneratorFromNumber(2);
  return CreateDhFromPQG(prime, generator, verify_error);
}

napi_value DiffieHellmanCtor(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value argv[2] = {nullptr, nullptr};
  napi_value this_arg = nullptr;
  if (napi_get_cb_info(env, info, &argc, argv, &this_arg, nullptr) != napi_ok || this_arg == nullptr) {
    return nullptr;
  }

  int32_t verify_error = 0;
  DH* dh = nullptr;
  napi_valuetype size_or_key_type = napi_undefined;
  if (argc >= 1 && argv[0] != nullptr) napi_typeof(env, argv[0], &size_or_key_type);

  int32_t generator_number = 2;
  std::vector<uint8_t> generator_bytes;
  if (argc >= 2 && argv[1] != nullptr && !IsNullOrUndefinedValue(env, argv[1])) {
    napi_valuetype generator_type = napi_undefined;
    if (napi_typeof(env, argv[1], &generator_type) == napi_ok && generator_type == napi_number) {
      generator_number = GetInt32Value(env, argv[1], 2);
    } else {
      generator_bytes = ValueToBytes(env, argv[1]);
    }
  }

  if (size_or_key_type == napi_number) {
    const int32_t bits = GetInt32Value(env, argv[0], 0);
    dh = CreateDhFromSize(bits, generator_number, generator_bytes, &verify_error);
  } else {
    const std::vector<uint8_t> prime_bytes = ValueToBytes(env, argv[0]);
    dh = CreateDhFromPrimeAndGenerator(prime_bytes, generator_number, generator_bytes, &verify_error);
  }

  if (dh == nullptr) {
    napi_throw_error(env, "ERR_CRYPTO_OPERATION_FAILED", "Diffie-Hellman initialization failed");
    return nullptr;
  }

  auto* wrap = new DiffieHellmanWrap();
  wrap->dh = dh;
  wrap->verify_error = verify_error;
  napi_wrap(env, this_arg, wrap, DiffieHellmanFinalize, nullptr, &wrap->wrapper_ref);
  SetNamedInt(env, this_arg, "verifyError", wrap->verify_error);
  return this_arg;
}

napi_value DiffieHellmanGroupCtor(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  napi_value this_arg = nullptr;
  if (napi_get_cb_info(env, info, &argc, argv, &this_arg, nullptr) != napi_ok || this_arg == nullptr) {
    return nullptr;
  }
  if (argc < 1 || argv[0] == nullptr) {
    napi_throw_error(env, "ERR_CRYPTO_UNKNOWN_DH_GROUP", "Unknown DH group");
    return nullptr;
  }
  const std::string group_name = GetStringValue(env, argv[0]);
  int32_t verify_error = 0;
  DH* dh = CreateDhFromGroupName(group_name, &verify_error);
  if (dh == nullptr) {
    napi_throw_error(env, "ERR_CRYPTO_UNKNOWN_DH_GROUP", "Unknown DH group");
    return nullptr;
  }

  auto* wrap = new DiffieHellmanWrap();
  wrap->dh = dh;
  wrap->verify_error = verify_error;
  napi_wrap(env, this_arg, wrap, DiffieHellmanFinalize, nullptr, &wrap->wrapper_ref);
  SetNamedInt(env, this_arg, "verifyError", wrap->verify_error);
  return this_arg;
}

napi_value DiffieHellmanGenerateKeys(napi_env env, napi_callback_info info) {
  napi_value this_arg = nullptr;
  size_t argc = 0;
  napi_get_cb_info(env, info, &argc, nullptr, &this_arg, nullptr);
  DiffieHellmanWrap* wrap = UnwrapDiffieHellman(env, this_arg);
  if (wrap == nullptr || wrap->dh == nullptr) return Undefined(env);
  if (DH_generate_key(wrap->dh) != 1) {
    napi_throw_error(env, "ERR_CRYPTO_OPERATION_FAILED", "Diffie-Hellman key generation failed");
    return nullptr;
  }
  const BIGNUM* public_key = nullptr;
  const BIGNUM* private_key = nullptr;
  DH_get0_key(wrap->dh, &public_key, &private_key);
  return BytesToBuffer(env, BigNumToBytes(public_key));
}

napi_value DiffieHellmanComputeSecret(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  napi_value this_arg = nullptr;
  napi_get_cb_info(env, info, &argc, argv, &this_arg, nullptr);
  DiffieHellmanWrap* wrap = UnwrapDiffieHellman(env, this_arg);
  if (wrap == nullptr || wrap->dh == nullptr || argc < 1 || argv[0] == nullptr) return Undefined(env);

  const std::vector<uint8_t> peer_bytes = ValueToBytes(env, argv[0]);
  BIGNUM* peer_key = BN_bin2bn(peer_bytes.data(), static_cast<int>(peer_bytes.size()), nullptr);
  if (peer_key == nullptr) {
    napi_throw_error(env, "ERR_INVALID_ARG_TYPE", "Invalid peer public key");
    return nullptr;
  }

  std::vector<uint8_t> secret(static_cast<size_t>(DH_size(wrap->dh)));
  const int secret_len = DH_compute_key(secret.data(), peer_key, wrap->dh);
  BN_free(peer_key);

  if (secret_len <= 0) {
    napi_value err_text = nullptr;
    napi_create_string_utf8(env, "invalid public key", NAPI_AUTO_LENGTH, &err_text);
    return err_text;
  }
  secret.resize(static_cast<size_t>(secret_len));
  return BytesToBuffer(env, secret);
}

napi_value DiffieHellmanGetPrime(napi_env env, napi_callback_info info) {
  napi_value this_arg = nullptr;
  size_t argc = 0;
  napi_get_cb_info(env, info, &argc, nullptr, &this_arg, nullptr);
  DiffieHellmanWrap* wrap = UnwrapDiffieHellman(env, this_arg);
  if (wrap == nullptr || wrap->dh == nullptr) return Undefined(env);
  const BIGNUM* prime = nullptr;
  const BIGNUM* q = nullptr;
  const BIGNUM* generator = nullptr;
  DH_get0_pqg(wrap->dh, &prime, &q, &generator);
  return BytesToBuffer(env, BigNumToBytes(prime));
}

napi_value DiffieHellmanGetGenerator(napi_env env, napi_callback_info info) {
  napi_value this_arg = nullptr;
  size_t argc = 0;
  napi_get_cb_info(env, info, &argc, nullptr, &this_arg, nullptr);
  DiffieHellmanWrap* wrap = UnwrapDiffieHellman(env, this_arg);
  if (wrap == nullptr || wrap->dh == nullptr) return Undefined(env);
  const BIGNUM* prime = nullptr;
  const BIGNUM* q = nullptr;
  const BIGNUM* generator = nullptr;
  DH_get0_pqg(wrap->dh, &prime, &q, &generator);
  return BytesToBuffer(env, BigNumToBytes(generator));
}

napi_value DiffieHellmanGetPublicKey(napi_env env, napi_callback_info info) {
  napi_value this_arg = nullptr;
  size_t argc = 0;
  napi_get_cb_info(env, info, &argc, nullptr, &this_arg, nullptr);
  DiffieHellmanWrap* wrap = UnwrapDiffieHellman(env, this_arg);
  if (wrap == nullptr || wrap->dh == nullptr) return Undefined(env);
  const BIGNUM* public_key = nullptr;
  const BIGNUM* private_key = nullptr;
  DH_get0_key(wrap->dh, &public_key, &private_key);
  return BytesToBuffer(env, BigNumToBytes(public_key));
}

napi_value DiffieHellmanGetPrivateKey(napi_env env, napi_callback_info info) {
  napi_value this_arg = nullptr;
  size_t argc = 0;
  napi_get_cb_info(env, info, &argc, nullptr, &this_arg, nullptr);
  DiffieHellmanWrap* wrap = UnwrapDiffieHellman(env, this_arg);
  if (wrap == nullptr || wrap->dh == nullptr) return Undefined(env);
  const BIGNUM* public_key = nullptr;
  const BIGNUM* private_key = nullptr;
  DH_get0_key(wrap->dh, &public_key, &private_key);
  return BytesToBuffer(env, BigNumToBytes(private_key));
}

napi_value DiffieHellmanSetPublicKey(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  napi_value this_arg = nullptr;
  napi_get_cb_info(env, info, &argc, argv, &this_arg, nullptr);
  DiffieHellmanWrap* wrap = UnwrapDiffieHellman(env, this_arg);
  if (wrap == nullptr || wrap->dh == nullptr || argc < 1 || argv[0] == nullptr) return Undefined(env);
  const std::vector<uint8_t> public_bytes = ValueToBytes(env, argv[0]);
  BIGNUM* new_public = BN_bin2bn(public_bytes.data(), static_cast<int>(public_bytes.size()), nullptr);
  if (new_public == nullptr) {
    napi_throw_error(env, "ERR_INVALID_ARG_TYPE", "Invalid public key");
    return nullptr;
  }
  const BIGNUM* current_public = nullptr;
  const BIGNUM* current_private = nullptr;
  DH_get0_key(wrap->dh, &current_public, &current_private);
  BIGNUM* private_copy = current_private != nullptr ? BN_dup(current_private) : nullptr;
  if (DH_set0_key(wrap->dh, new_public, private_copy) != 1) {
    BN_free(new_public);
    if (private_copy != nullptr) BN_free(private_copy);
    napi_throw_error(env, "ERR_CRYPTO_OPERATION_FAILED", "Failed to set public key");
    return nullptr;
  }
  return Undefined(env);
}

napi_value DiffieHellmanSetPrivateKey(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  napi_value this_arg = nullptr;
  napi_get_cb_info(env, info, &argc, argv, &this_arg, nullptr);
  DiffieHellmanWrap* wrap = UnwrapDiffieHellman(env, this_arg);
  if (wrap == nullptr || wrap->dh == nullptr || argc < 1 || argv[0] == nullptr) return Undefined(env);
  const std::vector<uint8_t> private_bytes = ValueToBytes(env, argv[0]);
  BIGNUM* new_private = BN_bin2bn(private_bytes.data(), static_cast<int>(private_bytes.size()), nullptr);
  if (new_private == nullptr) {
    napi_throw_error(env, "ERR_INVALID_ARG_TYPE", "Invalid private key");
    return nullptr;
  }
  const BIGNUM* current_public = nullptr;
  const BIGNUM* current_private = nullptr;
  DH_get0_key(wrap->dh, &current_public, &current_private);
  BIGNUM* public_copy = current_public != nullptr ? BN_dup(current_public) : nullptr;
  if (DH_set0_key(wrap->dh, public_copy, new_private) != 1) {
    if (public_copy != nullptr) BN_free(public_copy);
    BN_free(new_private);
    napi_throw_error(env, "ERR_CRYPTO_OPERATION_FAILED", "Failed to set private key");
    return nullptr;
  }
  return Undefined(env);
}

struct EcdhWrap {
  napi_ref wrapper_ref = nullptr;
  EC_KEY* ec = nullptr;
};

void EcdhFinalize(napi_env env, void* data, void* /*hint*/) {
  auto* wrap = static_cast<EcdhWrap*>(data);
  if (wrap == nullptr) return;
  if (wrap->ec != nullptr) {
    EC_KEY_free(wrap->ec);
    wrap->ec = nullptr;
  }
  ResetRef(env, &wrap->wrapper_ref);
  delete wrap;
}

EcdhWrap* UnwrapEcdh(napi_env env, napi_value this_arg) {
  if (this_arg == nullptr) return nullptr;
  void* data = nullptr;
  if (napi_unwrap(env, this_arg, &data) != napi_ok || data == nullptr) return nullptr;
  return static_cast<EcdhWrap*>(data);
}

napi_value EcdhCtor(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  napi_value this_arg = nullptr;
  if (napi_get_cb_info(env, info, &argc, argv, &this_arg, nullptr) != napi_ok || this_arg == nullptr) {
    return nullptr;
  }
  if (argc < 1 || argv[0] == nullptr) {
    napi_throw_error(env, "ERR_INVALID_ARG_TYPE", "Invalid ECDH curve");
    return nullptr;
  }
  const std::string curve = GetStringValue(env, argv[0]);
  int nid = OBJ_txt2nid(curve.c_str());
  if (nid == NID_undef) nid = OBJ_sn2nid(curve.c_str());
  if (nid == NID_undef) nid = OBJ_ln2nid(curve.c_str());
  if (nid == NID_undef) {
    napi_throw_type_error(env, "ERR_CRYPTO_INVALID_CURVE", "Invalid ECDH curve name");
    return nullptr;
  }
  EC_KEY* ec = EC_KEY_new_by_curve_name(nid);
  if (ec == nullptr) {
    napi_throw_error(env, "ERR_CRYPTO_OPERATION_FAILED", "ECDH initialization failed");
    return nullptr;
  }
  auto* wrap = new EcdhWrap();
  wrap->ec = ec;
  napi_wrap(env, this_arg, wrap, EcdhFinalize, nullptr, &wrap->wrapper_ref);
  return this_arg;
}

napi_value EcdhGenerateKeys(napi_env env, napi_callback_info info) {
  napi_value this_arg = nullptr;
  size_t argc = 0;
  napi_get_cb_info(env, info, &argc, nullptr, &this_arg, nullptr);
  EcdhWrap* wrap = UnwrapEcdh(env, this_arg);
  if (wrap == nullptr || wrap->ec == nullptr) return Undefined(env);
  if (EC_KEY_generate_key(wrap->ec) != 1) {
    napi_throw_error(env, "ERR_CRYPTO_OPERATION_FAILED", "ECDH key generation failed");
    return nullptr;
  }
  return Undefined(env);
}

napi_value EcdhGetPublicKey(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  napi_value this_arg = nullptr;
  napi_get_cb_info(env, info, &argc, argv, &this_arg, nullptr);
  EcdhWrap* wrap = UnwrapEcdh(env, this_arg);
  if (wrap == nullptr || wrap->ec == nullptr) return Undefined(env);
  int32_t form = POINT_CONVERSION_UNCOMPRESSED;
  if (argc >= 1 && argv[0] != nullptr && !IsUndefined(env, argv[0])) {
    form = GetInt32Value(env, argv[0], POINT_CONVERSION_UNCOMPRESSED);
  }
  point_conversion_form_t conversion = POINT_CONVERSION_UNCOMPRESSED;
  if (form == POINT_CONVERSION_COMPRESSED) {
    conversion = POINT_CONVERSION_COMPRESSED;
  } else if (form == POINT_CONVERSION_HYBRID) {
    conversion = POINT_CONVERSION_HYBRID;
  }
  const EC_GROUP* group = EC_KEY_get0_group(wrap->ec);
  const EC_POINT* point = EC_KEY_get0_public_key(wrap->ec);
  if (group == nullptr || point == nullptr) return BytesToBuffer(env, {});
  const size_t len = EC_POINT_point2oct(group, point, conversion, nullptr, 0, nullptr);
  std::vector<uint8_t> out(len);
  if (len == 0 ||
      EC_POINT_point2oct(group, point, conversion, out.data(), out.size(), nullptr) != len) {
    napi_throw_error(env, "ERR_CRYPTO_OPERATION_FAILED", "Failed to export ECDH public key");
    return nullptr;
  }
  return BytesToBuffer(env, out);
}

napi_value EcdhGetPrivateKey(napi_env env, napi_callback_info info) {
  napi_value this_arg = nullptr;
  size_t argc = 0;
  napi_get_cb_info(env, info, &argc, nullptr, &this_arg, nullptr);
  EcdhWrap* wrap = UnwrapEcdh(env, this_arg);
  if (wrap == nullptr || wrap->ec == nullptr) return Undefined(env);
  const BIGNUM* private_key = EC_KEY_get0_private_key(wrap->ec);
  return BytesToBuffer(env, BigNumToBytes(private_key));
}

napi_value EcdhSetPrivateKey(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  napi_value this_arg = nullptr;
  napi_get_cb_info(env, info, &argc, argv, &this_arg, nullptr);
  EcdhWrap* wrap = UnwrapEcdh(env, this_arg);
  if (wrap == nullptr || wrap->ec == nullptr || argc < 1 || argv[0] == nullptr) return Undefined(env);
  const std::vector<uint8_t> key_bytes = ValueToBytes(env, argv[0]);
  BIGNUM* private_key = BN_bin2bn(key_bytes.data(), static_cast<int>(key_bytes.size()), nullptr);
  if (private_key == nullptr) {
    napi_throw_error(env, "ERR_INVALID_ARG_TYPE", "Invalid private key");
    return nullptr;
  }
  if (EC_KEY_set_private_key(wrap->ec, private_key) != 1) {
    BN_free(private_key);
    napi_throw_error(env, "ERR_CRYPTO_OPERATION_FAILED", "Failed to set ECDH private key");
    return nullptr;
  }
  const EC_GROUP* group = EC_KEY_get0_group(wrap->ec);
  EC_POINT* public_key = group != nullptr ? EC_POINT_new(group) : nullptr;
  BN_CTX* bn_ctx = BN_CTX_new();
  if (group == nullptr ||
      public_key == nullptr ||
      bn_ctx == nullptr ||
      EC_POINT_mul(group, public_key, private_key, nullptr, nullptr, bn_ctx) != 1 ||
      EC_KEY_set_public_key(wrap->ec, public_key) != 1) {
    if (public_key != nullptr) EC_POINT_free(public_key);
    if (bn_ctx != nullptr) BN_CTX_free(bn_ctx);
    BN_free(private_key);
    napi_throw_error(env, "ERR_CRYPTO_OPERATION_FAILED", "Failed to derive ECDH public key");
    return nullptr;
  }
  EC_POINT_free(public_key);
  BN_CTX_free(bn_ctx);
  BN_free(private_key);
  return Undefined(env);
}

napi_value EcdhSetPublicKey(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  napi_value this_arg = nullptr;
  napi_get_cb_info(env, info, &argc, argv, &this_arg, nullptr);
  EcdhWrap* wrap = UnwrapEcdh(env, this_arg);
  if (wrap == nullptr || wrap->ec == nullptr || argc < 1 || argv[0] == nullptr) return Undefined(env);
  const std::vector<uint8_t> key_bytes = ValueToBytes(env, argv[0]);
  const EC_GROUP* group = EC_KEY_get0_group(wrap->ec);
  EC_POINT* public_key = group != nullptr ? EC_POINT_new(group) : nullptr;
  if (group == nullptr || public_key == nullptr ||
      EC_POINT_oct2point(group, public_key, key_bytes.data(), key_bytes.size(), nullptr) != 1 ||
      EC_KEY_set_public_key(wrap->ec, public_key) != 1) {
    if (public_key != nullptr) EC_POINT_free(public_key);
    napi_throw_error(env, "ERR_CRYPTO_OPERATION_FAILED", "Failed to set ECDH public key");
    return nullptr;
  }
  EC_POINT_free(public_key);
  return Undefined(env);
}

napi_value EcdhComputeSecret(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  napi_value this_arg = nullptr;
  napi_get_cb_info(env, info, &argc, argv, &this_arg, nullptr);
  EcdhWrap* wrap = UnwrapEcdh(env, this_arg);
  if (wrap == nullptr || wrap->ec == nullptr || argc < 1 || argv[0] == nullptr) return Undefined(env);
  const std::vector<uint8_t> peer_bytes = ValueToBytes(env, argv[0]);
  const EC_GROUP* group = EC_KEY_get0_group(wrap->ec);
  EC_POINT* peer_point = group != nullptr ? EC_POINT_new(group) : nullptr;
  if (group == nullptr ||
      peer_point == nullptr ||
      EC_POINT_oct2point(group, peer_point, peer_bytes.data(), peer_bytes.size(), nullptr) != 1) {
    if (peer_point != nullptr) EC_POINT_free(peer_point);
    napi_value err_text = nullptr;
    napi_create_string_utf8(env, "invalid public key", NAPI_AUTO_LENGTH, &err_text);
    return err_text;
  }
  const size_t secret_len = static_cast<size_t>((EC_GROUP_get_degree(group) + 7) / 8);
  std::vector<uint8_t> secret(secret_len);
  const int written = ECDH_compute_key(secret.data(), secret.size(), peer_point, wrap->ec, nullptr);
  EC_POINT_free(peer_point);
  if (written <= 0) {
    napi_value err_text = nullptr;
    napi_create_string_utf8(env, "invalid public key", NAPI_AUTO_LENGTH, &err_text);
    return err_text;
  }
  secret.resize(static_cast<size_t>(written));
  return BytesToBuffer(env, secret);
}

struct CipherBaseWrap {
  napi_ref wrapper_ref = nullptr;
  bool encrypt = true;
  std::string algorithm;
  std::vector<uint8_t> key;
  bool iv_is_null = false;
  std::vector<uint8_t> iv;
  std::vector<uint8_t> input;
  std::vector<uint8_t> aad;
  std::vector<uint8_t> auth_tag;
  int32_t auth_tag_length = 16;
  bool finalized = false;
};

struct KeyEncodingSelection {
  bool has_public_encoding = false;
  int32_t public_format = kKeyFormatPEM;
  int32_t public_type = kKeyEncodingSPKI;
  bool has_private_encoding = false;
  int32_t private_format = kKeyFormatPEM;
  int32_t private_type = kKeyEncodingPKCS8;
  std::string private_cipher;
  std::vector<uint8_t> private_passphrase;
  bool private_passphrase_provided = false;
};

bool ExportPublicKeyValue(napi_env env,
                          EVP_PKEY* pkey,
                          const KeyEncodingSelection& encoding,
                          const std::string& curve_name_hint,
                          napi_value* out_value,
                          std::string* error_code,
                          std::string* error_message);
bool ExportPrivateKeyValue(napi_env env,
                           EVP_PKEY* pkey,
                           const KeyEncodingSelection& encoding,
                           const std::string& curve_name_hint,
                           napi_value* out_value,
                           std::string* error_code,
                           std::string* error_message);

void CipherBaseFinalize(napi_env env, void* data, void* /*hint*/) {
  auto* wrap = static_cast<CipherBaseWrap*>(data);
  if (wrap == nullptr) return;
  ResetRef(env, &wrap->wrapper_ref);
  delete wrap;
}

CipherBaseWrap* UnwrapCipherBase(napi_env env, napi_value this_arg) {
  if (this_arg == nullptr) return nullptr;
  void* data = nullptr;
  if (napi_unwrap(env, this_arg, &data) != napi_ok || data == nullptr) return nullptr;
  return static_cast<CipherBaseWrap*>(data);
}

bool IsAeadCipherName(std::string name) {
  for (char& ch : name) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  return name.find("gcm") != std::string::npos ||
         name.find("ccm") != std::string::npos ||
         name.find("ocb") != std::string::npos ||
         name.find("chacha20-poly1305") != std::string::npos;
}

bool IsWrapCipherName(std::string name) {
  for (char& ch : name) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  return name.find("wrap") != std::string::npos;
}

napi_value CipherBaseCtor(napi_env env, napi_callback_info info) {
  size_t argc = 5;
  napi_value argv[5] = {nullptr, nullptr, nullptr, nullptr, nullptr};
  napi_value this_arg = nullptr;
  if (napi_get_cb_info(env, info, &argc, argv, &this_arg, nullptr) != napi_ok || this_arg == nullptr) {
    return nullptr;
  }
  auto* wrap = new CipherBaseWrap();
  if (argc >= 1 && argv[0] != nullptr) {
    bool encrypt = true;
    if (napi_get_value_bool(env, argv[0], &encrypt) == napi_ok) wrap->encrypt = encrypt;
  }
  if (argc >= 2 && argv[1] != nullptr) {
    size_t len = 0;
    if (napi_get_value_string_utf8(env, argv[1], nullptr, 0, &len) == napi_ok) {
      wrap->algorithm.resize(len + 1);
      size_t copied = 0;
      if (napi_get_value_string_utf8(env, argv[1], wrap->algorithm.data(), wrap->algorithm.size(), &copied) ==
          napi_ok) {
        wrap->algorithm.resize(copied);
      }
    }
  }
  if (argc >= 3 && argv[2] != nullptr) {
    wrap->key = ValueToBytes(env, argv[2]);
  }
  if (argc < 4 || argv[3] == nullptr || IsUndefined(env, argv[3])) {
    napi_throw(
        env,
        CreateErrorWithCode(env,
                            "ERR_INVALID_ARG_TYPE",
                            "The \"iv\" argument must be of type string, Buffer, ArrayBuffer, TypedArray, or DataView."));
    delete wrap;
    return nullptr;
  }
  if (argc >= 4 && argv[3] != nullptr) {
    napi_valuetype iv_type = napi_undefined;
    if (napi_typeof(env, argv[3], &iv_type) == napi_ok && iv_type == napi_null) {
      wrap->iv_is_null = true;
    } else {
      wrap->iv = ValueToBytes(env, argv[3]);
    }
  }
  if (argc >= 5 && argv[4] != nullptr) {
    napi_get_value_int32(env, argv[4], &wrap->auth_tag_length);
  }
  if (wrap->auth_tag_length <= 0 || wrap->auth_tag_length > 16) wrap->auth_tag_length = 16;

  // Validate cipher/key/iv shape at construction time without performing
  // a data transform, so valid ciphers do not fail early.
  napi_value binding = GetBinding(env);
  if (binding != nullptr) {
    napi_value opts = nullptr;
    napi_value algo = nullptr;
    if (napi_create_object(env, &opts) != napi_ok ||
        opts == nullptr ||
        napi_create_string_utf8(env, wrap->algorithm.c_str(), NAPI_AUTO_LENGTH, &algo) != napi_ok ||
        algo == nullptr) {
      delete wrap;
      return nullptr;
    }
    napi_value argv_info[2] = {opts, algo};
    napi_value info = nullptr;
    if (!CallBindingMethod(env, binding, "getCipherInfo", 2, argv_info, &info) ||
        info == nullptr ||
        IsUndefined(env, info)) {
      napi_throw(env, CreateErrorWithCode(env, "ERR_CRYPTO_UNKNOWN_CIPHER", "Unknown cipher"));
      delete wrap;
      return nullptr;
    }

    int32_t expected_key_len = -1;
    int32_t expected_iv_len = -1;
    GetNamedInt32Value(env, info, "keyLength", &expected_key_len);
    GetNamedInt32Value(env, info, "ivLength", &expected_iv_len);
    if (expected_iv_len < 0) {
      std::string mode;
      if (GetNamedStringValue(env, info, "mode", &mode) && mode == "ecb") {
        expected_iv_len = 0;
      }
    }

    if (expected_key_len > 0 && static_cast<int32_t>(wrap->key.size()) != expected_key_len) {
      napi_throw(env, CreateErrorWithCode(env, "ERR_CRYPTO_INVALID_KEYLEN", "Invalid key length"));
      delete wrap;
      return nullptr;
    }

    const int32_t actual_iv_len = wrap->iv_is_null ? 0 : static_cast<int32_t>(wrap->iv.size());
    if (IsAeadCipherName(wrap->algorithm)) {
      if (actual_iv_len <= 0) {
        napi_throw(env, CreateErrorWithCode(env, "ERR_CRYPTO_INVALID_IV", "Invalid initialization vector"));
        delete wrap;
        return nullptr;
      }
    } else if (expected_iv_len >= 0 && actual_iv_len != expected_iv_len) {
      napi_throw(env, CreateErrorWithCode(env, "ERR_CRYPTO_INVALID_IV", "Invalid initialization vector"));
      delete wrap;
      return nullptr;
    }
  }
  napi_wrap(env, this_arg, wrap, CipherBaseFinalize, nullptr, &wrap->wrapper_ref);
  return this_arg;
}

napi_value CipherBaseUpdate(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value argv[2] = {nullptr, nullptr};
  napi_value this_arg = nullptr;
  napi_get_cb_info(env, info, &argc, argv, &this_arg, nullptr);
  CipherBaseWrap* wrap = UnwrapCipherBase(env, this_arg);
  if (wrap == nullptr) return Undefined(env);
  if (wrap->finalized) return BytesToBuffer(env, {});
  if (argc >= 1 && argv[0] != nullptr) {
    std::vector<uint8_t> bytes = ValueToBytesWithEncoding(env, argv[0], argc >= 2 ? argv[1] : nullptr);
    wrap->input.insert(wrap->input.end(), bytes.begin(), bytes.end());
  }
  // RFC3394/5649 wrap algorithms emit their full output on update() in Node.
  if (IsWrapCipherName(wrap->algorithm)) {
    napi_value binding = GetBinding(env);
    if (binding == nullptr) return Undefined(env);
    napi_value algo = nullptr;
    napi_value key = BytesToBuffer(env, wrap->key);
    napi_value iv = wrap->iv_is_null ? nullptr : BytesToBuffer(env, wrap->iv);
    napi_value input = BytesToBuffer(env, wrap->input);
    napi_value decrypt = nullptr;
    napi_get_boolean(env, !wrap->encrypt, &decrypt);
    napi_value iv_arg = nullptr;
    if (wrap->iv_is_null) {
      napi_get_null(env, &iv_arg);
    } else {
      iv_arg = (iv != nullptr ? iv : Undefined(env));
    }
    napi_create_string_utf8(env, wrap->algorithm.c_str(), NAPI_AUTO_LENGTH, &algo);
    napi_value argv_transform[6] = {algo != nullptr ? algo : Undefined(env),
                                    key != nullptr ? key : Undefined(env),
                                    iv_arg,
                                    input != nullptr ? input : Undefined(env),
                                    decrypt != nullptr ? decrypt : Undefined(env),
                                    Undefined(env)};
    napi_value out = nullptr;
    if (!CallBindingMethod(env, binding, "cipherTransform", 6, argv_transform, &out)) return nullptr;
    wrap->input.clear();
    wrap->finalized = true;
    return EnsureBufferValue(env, out);
  }
  return BytesToBuffer(env, {});
}

napi_value CipherBaseFinal(napi_env env, napi_callback_info info) {
  napi_value this_arg = nullptr;
  size_t argc = 0;
  napi_get_cb_info(env, info, &argc, nullptr, &this_arg, nullptr);
  CipherBaseWrap* wrap = UnwrapCipherBase(env, this_arg);
  if (wrap == nullptr) return Undefined(env);
  if (wrap->finalized) return BytesToBuffer(env, {});
  wrap->finalized = true;

  napi_value binding = GetBinding(env);
  if (binding == nullptr) return Undefined(env);

  napi_value algo = nullptr;
  napi_create_string_utf8(env, wrap->algorithm.c_str(), NAPI_AUTO_LENGTH, &algo);
  napi_value key = BytesToBuffer(env, wrap->key);
  napi_value iv = wrap->iv_is_null ? nullptr : BytesToBuffer(env, wrap->iv);
  napi_value input = BytesToBuffer(env, wrap->input);
  napi_value decrypt = nullptr;
  napi_get_boolean(env, !wrap->encrypt, &decrypt);

  napi_value output = nullptr;
  if (IsAeadCipherName(wrap->algorithm)) {
    napi_value aad = BytesToBuffer(env, wrap->aad);
    napi_value auth_tag = wrap->auth_tag.empty() ? Undefined(env) : BytesToBuffer(env, wrap->auth_tag);
    napi_value auth_tag_len = nullptr;
    napi_create_int32(env, wrap->auth_tag_length, &auth_tag_len);
    napi_value argv_aead[8] = {
        algo != nullptr ? algo : Undefined(env),  key != nullptr ? key : Undefined(env),
        iv != nullptr ? iv : Undefined(env),      input != nullptr ? input : Undefined(env),
        decrypt != nullptr ? decrypt : Undefined(env), aad != nullptr ? aad : Undefined(env),
        auth_tag != nullptr ? auth_tag : Undefined(env), auth_tag_len != nullptr ? auth_tag_len : Undefined(env)};
    napi_value out = nullptr;
    if (!CallBindingMethod(env, binding, "cipherTransformAead", 8, argv_aead, &out) || out == nullptr) {
      return nullptr;
    }
    napi_value out_buf = nullptr;
    if (napi_get_named_property(env, out, "output", &out_buf) != napi_ok || out_buf == nullptr) {
      return nullptr;
    }
    if (wrap->encrypt) {
      napi_value out_tag = nullptr;
      if (napi_get_named_property(env, out, "authTag", &out_tag) == napi_ok && out_tag != nullptr &&
          !IsUndefined(env, out_tag)) {
        wrap->auth_tag = ValueToBytes(env, out_tag);
      }
    }
    output = EnsureBufferValue(env, out_buf);
  } else {
    napi_value iv_arg = nullptr;
    if (wrap->iv_is_null) {
      napi_get_null(env, &iv_arg);
    } else {
      iv_arg = (iv != nullptr ? iv : Undefined(env));
    }
    napi_value argv_transform[6] = {algo != nullptr ? algo : Undefined(env),
                                    key != nullptr ? key : Undefined(env),
                                    iv_arg,
                                    input != nullptr ? input : Undefined(env),
                                    decrypt != nullptr ? decrypt : Undefined(env),
                                    Undefined(env)};
    napi_value out = nullptr;
    if (!CallBindingMethod(env, binding, "cipherTransform", 6, argv_transform, &out)) return nullptr;
    output = EnsureBufferValue(env, out);
  }

  return output != nullptr ? output : Undefined(env);
}

napi_value CipherBaseSetAutoPadding(napi_env env, napi_callback_info info) {
  napi_value out = nullptr;
  napi_get_boolean(env, true, &out);
  return out != nullptr ? out : Undefined(env);
}

napi_value CipherBaseSetAAD(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value argv[2] = {nullptr, nullptr};
  napi_value this_arg = nullptr;
  napi_get_cb_info(env, info, &argc, argv, &this_arg, nullptr);
  CipherBaseWrap* wrap = UnwrapCipherBase(env, this_arg);
  if (wrap == nullptr) return Undefined(env);
  wrap->aad.clear();
  if (argc >= 1 && argv[0] != nullptr) {
    wrap->aad = ValueToBytes(env, argv[0]);
  }
  napi_value out = nullptr;
  napi_get_boolean(env, true, &out);
  return out != nullptr ? out : Undefined(env);
}

napi_value CipherBaseGetAuthTag(napi_env env, napi_callback_info info) {
  napi_value this_arg = nullptr;
  size_t argc = 0;
  napi_get_cb_info(env, info, &argc, nullptr, &this_arg, nullptr);
  CipherBaseWrap* wrap = UnwrapCipherBase(env, this_arg);
  if (wrap == nullptr || wrap->auth_tag.empty()) return Undefined(env);
  return BytesToBuffer(env, wrap->auth_tag);
}

napi_value CipherBaseSetAuthTag(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  napi_value this_arg = nullptr;
  napi_get_cb_info(env, info, &argc, argv, &this_arg, nullptr);
  CipherBaseWrap* wrap = UnwrapCipherBase(env, this_arg);
  if (wrap == nullptr) return Undefined(env);
  wrap->auth_tag.clear();
  if (argc >= 1 && argv[0] != nullptr) {
    wrap->auth_tag = ValueToBytes(env, argv[0]);
  }
  napi_value out = nullptr;
  napi_get_boolean(env, true, &out);
  return out != nullptr ? out : Undefined(env);
}

struct KeyObjectWrap {
  napi_ref wrapper_ref = nullptr;
  int32_t key_type = kKeyTypeSecret;
  std::vector<uint8_t> key_data;
  std::vector<uint8_t> key_passphrase;
  bool has_key_passphrase = false;
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
  size_t argc = 5;
  napi_value argv[5] = {nullptr, nullptr, nullptr, nullptr, nullptr};
  napi_value this_arg = nullptr;
  napi_get_cb_info(env, info, &argc, argv, &this_arg, nullptr);
  KeyObjectWrap* wrap = UnwrapKeyObject(env, this_arg);
  if (wrap == nullptr) return Undefined(env);
  int32_t key_type = kKeyTypeSecret;
  if (argc >= 1 && argv[0] != nullptr) napi_get_value_int32(env, argv[0], &key_type);
  wrap->key_type = key_type;
  wrap->key_data = (argc >= 2 && argv[1] != nullptr) ? ValueToBytes(env, argv[1]) : std::vector<uint8_t>{};
  wrap->key_passphrase.clear();
  wrap->has_key_passphrase = false;
  if (argc >= 5 && !IsNullOrUndefinedValue(env, argv[4])) {
    wrap->key_passphrase = ValueToBytes(env, argv[4]);
    wrap->has_key_passphrase = true;
  }
  napi_value out = nullptr;
  napi_get_boolean(env, true, &out);
  return out != nullptr ? out : Undefined(env);
}

napi_value KeyObjectExport(napi_env env, napi_callback_info info) {
  size_t argc = 4;
  napi_value argv[4] = {nullptr, nullptr, nullptr, nullptr};
  napi_value this_arg = nullptr;
  napi_get_cb_info(env, info, &argc, argv, &this_arg, nullptr);
  KeyObjectWrap* wrap = UnwrapKeyObject(env, this_arg);
  if (wrap == nullptr) return Undefined(env);
  if (wrap->key_type == kKeyTypeSecret) return BytesToBuffer(env, wrap->key_data);

  EVP_PKEY* pkey = ParseAnyKeyBytes(wrap->key_data);
  if (pkey == nullptr) return BytesToBuffer(env, wrap->key_data);

  KeyEncodingSelection encoding;
  std::string error_code;
  std::string error_message;
  napi_value out = nullptr;
  if (wrap->key_type == kKeyTypePublic) {
    encoding.has_public_encoding = true;
    encoding.public_format = argc >= 1 ? GetInt32Value(env, argv[0], kKeyFormatPEM) : kKeyFormatPEM;
    encoding.public_type = argc >= 2 ? GetInt32Value(env, argv[1], kKeyEncodingSPKI) : kKeyEncodingSPKI;
    if (!ExportPublicKeyValue(env, pkey, encoding, "", &out, &error_code, &error_message)) {
      EVP_PKEY_free(pkey);
      if (error_code.empty()) error_code = "ERR_CRYPTO_OPERATION_FAILED";
      if (error_message.empty()) error_message = "Public key export failed";
      napi_throw(env, CreateErrorWithCode(env, error_code.c_str(), error_message));
      return nullptr;
    }
  } else {
    encoding.has_private_encoding = true;
    encoding.private_format = argc >= 1 ? GetInt32Value(env, argv[0], kKeyFormatPEM) : kKeyFormatPEM;
    encoding.private_type = argc >= 2 ? GetInt32Value(env, argv[1], kKeyEncodingPKCS8) : kKeyEncodingPKCS8;
    if (argc >= 3 && !IsNullOrUndefinedValue(env, argv[2])) encoding.private_cipher = GetStringValue(env, argv[2]);
    if (argc >= 4 && !IsNullOrUndefinedValue(env, argv[3])) {
      encoding.private_passphrase = ValueToBytes(env, argv[3]);
      encoding.private_passphrase_provided = true;
    }
    if (!ExportPrivateKeyValue(env, pkey, encoding, "", &out, &error_code, &error_message)) {
      EVP_PKEY_free(pkey);
      if (error_code.empty()) error_code = "ERR_CRYPTO_OPERATION_FAILED";
      if (error_message.empty()) error_message = "Private key export failed";
      napi_throw(env, CreateErrorWithCode(env, error_code.c_str(), error_message));
      return nullptr;
    }
  }
  EVP_PKEY_free(pkey);
  return out != nullptr ? out : Undefined(env);
}

napi_value KeyObjectGetAsymmetricKeyType(napi_env env, napi_callback_info info) {
  napi_value this_arg = nullptr;
  size_t argc = 0;
  napi_get_cb_info(env, info, &argc, nullptr, &this_arg, nullptr);
  KeyObjectWrap* wrap = UnwrapKeyObject(env, this_arg);
  if (wrap == nullptr) return Undefined(env);
  napi_value binding = GetBinding(env);
  if (binding == nullptr) return Undefined(env);
  napi_value key = BytesToBuffer(env, wrap->key_data);
  napi_value passphrase = wrap->has_key_passphrase ? BytesToBuffer(env, wrap->key_passphrase) : Undefined(env);
  napi_value argv[2] = {key != nullptr ? key : Undefined(env), passphrase != nullptr ? passphrase : Undefined(env)};
  napi_value out = nullptr;
  if (!CallBindingMethod(env, binding, "getAsymmetricKeyType", 2, argv, &out)) return Undefined(env);
  return out != nullptr ? out : Undefined(env);
}

napi_value KeyObjectGetAsymmetricKeyDetails(napi_env env, napi_callback_info info) {
  napi_value this_arg = nullptr;
  size_t argc = 0;
  napi_get_cb_info(env, info, &argc, nullptr, &this_arg, nullptr);
  KeyObjectWrap* wrap = UnwrapKeyObject(env, this_arg);
  if (wrap == nullptr) return Undefined(env);
  napi_value binding = GetBinding(env);
  if (binding == nullptr) return Undefined(env);
  napi_value key = BytesToBuffer(env, wrap->key_data);
  napi_value passphrase = wrap->has_key_passphrase ? BytesToBuffer(env, wrap->key_passphrase) : Undefined(env);
  napi_value argv[2] = {key != nullptr ? key : Undefined(env), passphrase != nullptr ? passphrase : Undefined(env)};
  napi_value out = nullptr;
  if (!CallBindingMethod(env, binding, "getAsymmetricKeyDetails", 2, argv, &out) || out == nullptr) {
    napi_value empty = nullptr;
    napi_create_object(env, &empty);
    return empty != nullptr ? empty : Undefined(env);
  }
  return out;
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

SecureContextWrap* RequireSecureContext(napi_env env, napi_value this_arg) {
  SecureContextWrap* wrap = UnwrapSecureContext(env, this_arg);
  if (wrap != nullptr) return wrap;
  napi_throw_type_error(env, nullptr, "Illegal invocation");
  return nullptr;
}

void SecureContextCallBindingMethod(napi_env env,
                                    SecureContextWrap* wrap,
                                    const char* method,
                                    size_t extra_argc,
                                    napi_value* extra_argv) {
  if (wrap == nullptr || method == nullptr) return;
  napi_value binding = GetBinding(env);
  napi_value handle = GetRefValue(env, wrap->handle_ref);
  if (binding == nullptr || handle == nullptr) return;
  std::vector<napi_value> call_argv;
  call_argv.reserve(1 + extra_argc);
  call_argv.push_back(handle);
  for (size_t i = 0; i < extra_argc; ++i) {
    call_argv.push_back(extra_argv != nullptr ? extra_argv[i] : Undefined(env));
  }
  napi_value ignored = nullptr;
  CallBindingMethod(env, binding, method, call_argv.size(), call_argv.data(), &ignored);
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
  SecureContextWrap* wrap = RequireSecureContext(env, this_arg);
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
  SecureContextWrap* wrap = RequireSecureContext(env, this_arg);
  if (wrap == nullptr) return Undefined(env);
  napi_value call_argv[1] = {argc >= 1 ? argv[0] : Undefined(env)};
  SecureContextCallBindingMethod(env, wrap, "secureContextSetOptions", 1, call_argv);
  return Undefined(env);
}

napi_value SecureContextSetCiphers(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  napi_value this_arg = nullptr;
  napi_get_cb_info(env, info, &argc, argv, &this_arg, nullptr);
  SecureContextWrap* wrap = RequireSecureContext(env, this_arg);
  if (wrap == nullptr) return Undefined(env);
  napi_value call_argv[1] = {argc >= 1 ? argv[0] : Undefined(env)};
  SecureContextCallBindingMethod(env, wrap, "secureContextSetCiphers", 1, call_argv);
  return Undefined(env);
}

napi_value SecureContextSetCipherSuites(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  napi_value this_arg = nullptr;
  napi_get_cb_info(env, info, &argc, argv, &this_arg, nullptr);
  SecureContextWrap* wrap = RequireSecureContext(env, this_arg);
  if (wrap == nullptr) return Undefined(env);
  napi_value call_argv[1] = {argc >= 1 ? argv[0] : Undefined(env)};
  SecureContextCallBindingMethod(env, wrap, "secureContextSetCipherSuites", 1, call_argv);
  return Undefined(env);
}

napi_value SecureContextSetCert(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  napi_value this_arg = nullptr;
  napi_get_cb_info(env, info, &argc, argv, &this_arg, nullptr);
  SecureContextWrap* wrap = RequireSecureContext(env, this_arg);
  if (wrap == nullptr) return Undefined(env);
  napi_value call_argv[1] = {argc >= 1 ? argv[0] : Undefined(env)};
  SecureContextCallBindingMethod(env, wrap, "secureContextSetCert", 1, call_argv);
  return Undefined(env);
}

napi_value SecureContextSetKey(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value argv[2] = {nullptr, nullptr};
  napi_value this_arg = nullptr;
  napi_get_cb_info(env, info, &argc, argv, &this_arg, nullptr);
  SecureContextWrap* wrap = RequireSecureContext(env, this_arg);
  if (wrap == nullptr) return Undefined(env);
  napi_value call_argv[2] = {argc >= 1 ? argv[0] : Undefined(env), argc >= 2 ? argv[1] : Undefined(env)};
  SecureContextCallBindingMethod(env, wrap, "secureContextSetKey", 2, call_argv);
  return Undefined(env);
}

napi_value SecureContextAddCACert(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  napi_value this_arg = nullptr;
  napi_get_cb_info(env, info, &argc, argv, &this_arg, nullptr);
  SecureContextWrap* wrap = RequireSecureContext(env, this_arg);
  if (wrap == nullptr) return Undefined(env);
  napi_value call_argv[1] = {argc >= 1 ? argv[0] : Undefined(env)};
  SecureContextCallBindingMethod(env, wrap, "secureContextAddCACert", 1, call_argv);
  return Undefined(env);
}

napi_value SecureContextAddCrl(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  napi_value this_arg = nullptr;
  napi_get_cb_info(env, info, &argc, argv, &this_arg, nullptr);
  SecureContextWrap* wrap = RequireSecureContext(env, this_arg);
  if (wrap == nullptr) return Undefined(env);
  napi_value call_argv[1] = {argc >= 1 ? argv[0] : Undefined(env)};
  SecureContextCallBindingMethod(env, wrap, "secureContextAddCrl", 1, call_argv);
  return Undefined(env);
}

napi_value SecureContextNoop(napi_env env, napi_callback_info info) {
  napi_value this_arg = nullptr;
  size_t argc = 0;
  napi_get_cb_info(env, info, &argc, nullptr, &this_arg, nullptr);
  SecureContextWrap* wrap = RequireSecureContext(env, this_arg);
  if (wrap == nullptr) return Undefined(env);
  return Undefined(env);
}

napi_value SecureContextSetMinProto(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  napi_value this_arg = nullptr;
  napi_get_cb_info(env, info, &argc, argv, &this_arg, nullptr);
  SecureContextWrap* wrap = RequireSecureContext(env, this_arg);
  if (wrap == nullptr) return Undefined(env);
  int32_t value = 0;
  if (argc >= 1 && argv[0] != nullptr) napi_get_value_int32(env, argv[0], &value);
  wrap->min_proto = value;
  return Undefined(env);
}

napi_value SecureContextSetMaxProto(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  napi_value this_arg = nullptr;
  napi_get_cb_info(env, info, &argc, argv, &this_arg, nullptr);
  SecureContextWrap* wrap = RequireSecureContext(env, this_arg);
  if (wrap == nullptr) return Undefined(env);
  int32_t value = 0;
  if (argc >= 1 && argv[0] != nullptr) napi_get_value_int32(env, argv[0], &value);
  wrap->max_proto = value;
  return Undefined(env);
}

napi_value SecureContextLoadPKCS12(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value argv[2] = {nullptr, nullptr};
  napi_value this_arg = nullptr;
  napi_get_cb_info(env, info, &argc, argv, &this_arg, nullptr);
  SecureContextWrap* wrap = RequireSecureContext(env, this_arg);
  if (wrap == nullptr) return Undefined(env);

  napi_value binding = GetBinding(env);
  if (binding == nullptr) return Undefined(env);
  napi_value ignored = nullptr;
  if (!CallBindingMethod(env, binding, "parsePfx", argc, argv, &ignored)) return nullptr;
  return Undefined(env);
}

napi_value SecureContextGetMinProto(napi_env env, napi_callback_info info) {
  napi_value this_arg = nullptr;
  size_t argc = 0;
  napi_get_cb_info(env, info, &argc, nullptr, &this_arg, nullptr);
  SecureContextWrap* wrap = RequireSecureContext(env, this_arg);
  if (wrap == nullptr) return Undefined(env);
  napi_value out = nullptr;
  napi_create_int32(env, wrap->min_proto, &out);
  return out != nullptr ? out : Undefined(env);
}

napi_value SecureContextGetMaxProto(napi_env env, napi_callback_info info) {
  napi_value this_arg = nullptr;
  size_t argc = 0;
  napi_get_cb_info(env, info, &argc, nullptr, &this_arg, nullptr);
  SecureContextWrap* wrap = RequireSecureContext(env, this_arg);
  if (wrap == nullptr) return Undefined(env);
  napi_value out = nullptr;
  napi_create_int32(env, wrap->max_proto, &out);
  return out != nullptr ? out : Undefined(env);
}

napi_value SecureContextClose(napi_env env, napi_callback_info info) {
  napi_value this_arg = nullptr;
  size_t argc = 0;
  napi_get_cb_info(env, info, &argc, nullptr, &this_arg, nullptr);
  SecureContextWrap* wrap = RequireSecureContext(env, this_arg);
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

void MaybeAttachCurrentDomain(napi_env env, napi_value target);

bool FinalizeJobCtor(napi_env env, napi_value this_arg, size_t argc, napi_value* argv) {
  if (this_arg == nullptr) return false;
  auto* wrap = new JobWrap();
  if (argc >= 1 && argv[0] != nullptr) napi_get_value_int32(env, argv[0], &wrap->mode);
  for (size_t i = 1; i < argc; ++i) {
    napi_ref ref = nullptr;
    if (argv[i] != nullptr) napi_create_reference(env, argv[i], 1, &ref);
    wrap->args.push_back(ref);
  }
  if (napi_wrap(env, this_arg, wrap, JobFinalize, nullptr, &wrap->wrapper_ref) != napi_ok) {
    JobFinalize(env, wrap, nullptr);
    return false;
  }
  if (wrap->mode == kCryptoJobAsync) {
    MaybeAttachCurrentDomain(env, this_arg);
  }
  return true;
}

napi_value JobCtor(napi_env env, napi_callback_info info) {
  size_t argc = 16;
  napi_value argv[16] = {nullptr};
  napi_value this_arg = nullptr;
  napi_get_cb_info(env, info, &argc, argv, &this_arg, nullptr);
  if (this_arg == nullptr) return nullptr;
  if (!FinalizeJobCtor(env, this_arg, argc, argv)) return nullptr;
  return this_arg;
}

napi_value PBKDF2JobCtor(napi_env env, napi_callback_info info) {
  size_t argc = 16;
  napi_value argv[16] = {nullptr};
  napi_value this_arg = nullptr;
  napi_get_cb_info(env, info, &argc, argv, &this_arg, nullptr);
  if (this_arg == nullptr) return nullptr;

  if (argc >= 6 && argv[5] != nullptr && !IsNullOrUndefinedValue(env, argv[5])) {
    napi_value binding = GetBinding(env);
    if (!IsSupportedDigestName(env, binding, argv[5])) {
      const std::string digest = GetStringValue(env, argv[5]);
      const std::string message = "Invalid digest: " + digest;
      napi_throw_type_error(env, "ERR_CRYPTO_INVALID_DIGEST", message.c_str());
      return nullptr;
    }
  }

  if (!FinalizeJobCtor(env, this_arg, argc, argv)) return nullptr;
  return this_arg;
}

napi_value ScryptJobCtor(napi_env env, napi_callback_info info) {
  size_t argc = 16;
  napi_value argv[16] = {nullptr};
  napi_value this_arg = nullptr;
  napi_get_cb_info(env, info, &argc, argv, &this_arg, nullptr);
  if (this_arg == nullptr) return nullptr;

  if (argc >= 8) {
    auto to_u64 = [&](napi_value value) -> uint64_t {
      if (value == nullptr || IsUndefined(env, value)) return 0;
      double v = 0;
      if (napi_get_value_double(env, value, &v) != napi_ok || !std::isfinite(v) || v < 0) return 0;
      return static_cast<uint64_t>(v);
    };

    const uint64_t N = to_u64(argv[3]);
    const uint64_t r = to_u64(argv[4]);
    const uint64_t p = to_u64(argv[5]);
    const uint64_t maxmem = to_u64(argv[6]);
    if (!ncrypto::checkScryptParams(N, r, p, maxmem)) {
      const uint32_t err = ERR_peek_last_error();
      if (err != 0) {
        char buf[256];
        ERR_error_string_n(err, buf, sizeof(buf));
        const std::string message = std::string("Invalid scrypt params: ") + buf;
        napi_throw_error(env, "ERR_CRYPTO_INVALID_SCRYPT_PARAMS", message.c_str());
      } else {
        napi_throw_error(env, "ERR_CRYPTO_INVALID_SCRYPT_PARAMS", "Invalid scrypt params");
      }
      return nullptr;
    }
  }

  if (!FinalizeJobCtor(env, this_arg, argc, argv)) return nullptr;
  return this_arg;
}

napi_value HKDFJobCtor(napi_env env, napi_callback_info info) {
  size_t argc = 16;
  napi_value argv[16] = {nullptr};
  napi_value this_arg = nullptr;
  napi_get_cb_info(env, info, &argc, argv, &this_arg, nullptr);
  if (this_arg == nullptr) return nullptr;

  if (argc >= 2 && argv[1] != nullptr && !IsNullOrUndefinedValue(env, argv[1])) {
    napi_value binding = GetBinding(env);
    if (!IsSupportedDigestName(env, binding, argv[1])) {
      const std::string digest = GetStringValue(env, argv[1]);
      const std::string message = "Invalid digest: " + digest;
      napi_throw_type_error(env, "ERR_CRYPTO_INVALID_DIGEST", message.c_str());
      return nullptr;
    }

    if (argc >= 6 && argv[5] != nullptr && !IsNullOrUndefinedValue(env, argv[5])) {
      uint32_t length = 0;
      if (napi_get_value_uint32(env, argv[5], &length) == napi_ok) {
        const std::string digest = GetStringValue(env, argv[1]);
        const ncrypto::Digest md = ncrypto::Digest::FromName(digest.c_str());
        if (md && !ncrypto::checkHkdfLength(md, static_cast<size_t>(length))) {
          napi_throw_range_error(env, "ERR_CRYPTO_INVALID_KEYLEN", "Invalid key length");
          return nullptr;
        }
      }
    }
  }

  if (!FinalizeJobCtor(env, this_arg, argc, argv)) return nullptr;
  return this_arg;
}

napi_value RsaKeyPairGenJobCtor(napi_env env, napi_callback_info info) {
  size_t argc = 16;
  napi_value argv[16] = {nullptr};
  napi_value this_arg = nullptr;
  napi_get_cb_info(env, info, &argc, argv, &this_arg, nullptr);
  if (this_arg == nullptr) return nullptr;

  const int32_t variant = (argc >= 2 && argv[1] != nullptr)
                              ? GetInt32Value(env, argv[1], kKeyVariantRSA_SSA_PKCS1_v1_5)
                              : kKeyVariantRSA_SSA_PKCS1_v1_5;
  if (variant == kKeyVariantRSA_PSS) {
    napi_value binding = GetBinding(env);
    if (binding != nullptr && argc >= 5 && argv[4] != nullptr && !IsNullOrUndefinedValue(env, argv[4])) {
      if (!IsSupportedDigestName(env, binding, argv[4])) {
        const std::string digest = GetStringValue(env, argv[4]);
        const std::string message = "Invalid digest: " + digest;
        napi_throw_type_error(env, "ERR_CRYPTO_INVALID_DIGEST", message.c_str());
        return nullptr;
      }
    }
    if (binding != nullptr && argc >= 6 && argv[5] != nullptr && !IsNullOrUndefinedValue(env, argv[5])) {
      if (!IsSupportedDigestName(env, binding, argv[5])) {
        const std::string digest = GetStringValue(env, argv[5]);
        const std::string message = "Invalid MGF1 digest: " + digest;
        napi_throw_type_error(env, "ERR_CRYPTO_INVALID_DIGEST", message.c_str());
        return nullptr;
      }
    }
  }

  auto* wrap = new JobWrap();
  if (argc >= 1 && argv[0] != nullptr) napi_get_value_int32(env, argv[0], &wrap->mode);
  for (size_t i = 1; i < argc; ++i) {
    napi_ref ref = nullptr;
    if (argv[i] != nullptr) napi_create_reference(env, argv[i], 1, &ref);
    wrap->args.push_back(ref);
  }
  napi_wrap(env, this_arg, wrap, JobFinalize, nullptr, &wrap->wrapper_ref);
  if (wrap->mode == kCryptoJobAsync) {
    MaybeAttachCurrentDomain(env, this_arg);
  }
  return this_arg;
}

napi_value DhKeyPairGenJobCtor(napi_env env, napi_callback_info info) {
  size_t argc = 16;
  napi_value argv[16] = {nullptr};
  napi_value this_arg = nullptr;
  napi_get_cb_info(env, info, &argc, argv, &this_arg, nullptr);
  if (this_arg == nullptr) return nullptr;

  if (argc >= 2 && argv[1] != nullptr) {
    napi_valuetype source_type = napi_undefined;
    if (napi_typeof(env, argv[1], &source_type) == napi_ok && source_type == napi_string) {
      const std::string group_name = GetStringValue(env, argv[1]);
      BIGNUM* prime = DhPrimeFromGroupName(group_name);
      if (prime == nullptr) {
        napi_throw_error(env, "ERR_CRYPTO_UNKNOWN_DH_GROUP", "Unknown DH group");
        return nullptr;
      }
      BN_free(prime);
    }
  }

  auto* wrap = new JobWrap();
  if (argc >= 1 && argv[0] != nullptr) napi_get_value_int32(env, argv[0], &wrap->mode);
  for (size_t i = 1; i < argc; ++i) {
    napi_ref ref = nullptr;
    if (argv[i] != nullptr) napi_create_reference(env, argv[i], 1, &ref);
    wrap->args.push_back(ref);
  }
  napi_wrap(env, this_arg, wrap, JobFinalize, nullptr, &wrap->wrapper_ref);
  if (wrap->mode == kCryptoJobAsync) {
    MaybeAttachCurrentDomain(env, this_arg);
  }
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

bool ThrowSyncJobErrorIfPresent(napi_env env, napi_value result) {
  if (result == nullptr) return false;
  napi_value err = nullptr;
  if (napi_get_element(env, result, 0, &err) != napi_ok || err == nullptr) return false;
  napi_valuetype type = napi_undefined;
  if (napi_typeof(env, err, &type) != napi_ok || type == napi_undefined || type == napi_null) {
    return false;
  }
  napi_throw(env, err);
  return true;
}

void MaybeAttachCurrentDomain(napi_env env, napi_value target) {
  if (env == nullptr || target == nullptr) return;

  napi_value global = GetGlobal(env);
  if (global == nullptr) return;

  napi_value process = nullptr;
  if (napi_get_named_property(env, global, "process", &process) != napi_ok || process == nullptr) return;

  napi_value domain = nullptr;
  if (napi_get_named_property(env, process, "domain", &domain) != napi_ok ||
      domain == nullptr ||
      IsNullOrUndefinedValue(env, domain)) {
    return;
  }

  napi_property_descriptor desc{
      "domain",
      nullptr,
      nullptr,
      nullptr,
      nullptr,
      domain,
      static_cast<napi_property_attributes>(napi_writable | napi_configurable),
      nullptr};
  (void)napi_define_properties(env, target, 1, &desc);
}

struct CryptoOnDoneTask {
  napi_env env = nullptr;
  napi_ref this_arg_ref = nullptr;
  napi_ref ondone_ref = nullptr;
  napi_ref err_ref = nullptr;
  napi_ref value_ref = nullptr;
};

void CleanupCryptoOnDoneTask(napi_env env, void* data) {
  auto* task = static_cast<CryptoOnDoneTask*>(data);
  if (task == nullptr) return;
  napi_env cleanup_env = env != nullptr ? env : task->env;
  if (cleanup_env != nullptr) {
    ResetRef(cleanup_env, &task->this_arg_ref);
    ResetRef(cleanup_env, &task->ondone_ref);
    ResetRef(cleanup_env, &task->err_ref);
    ResetRef(cleanup_env, &task->value_ref);
  }
  delete task;
}

void RunCryptoOnDoneTask(napi_env env, void* data) {
  auto* task = static_cast<CryptoOnDoneTask*>(data);
  if (task == nullptr) return;

  napi_value this_arg = GetRefValue(env, task->this_arg_ref);
  napi_value ondone = GetRefValue(env, task->ondone_ref);
  napi_value err = GetRefValue(env, task->err_ref);
  napi_value value = GetRefValue(env, task->value_ref);
  if (this_arg == nullptr || ondone == nullptr) return;

  napi_value argv[2] = {err != nullptr ? err : Undefined(env), value != nullptr ? value : Undefined(env)};
  napi_value ignored = nullptr;
  (void)UbiAsyncWrapMakeCallback(
      env, 0, this_arg, this_arg, ondone, 2, argv, &ignored, kUbiMakeCallbackNone);
}

void InvokeJobOnDone(napi_env env, napi_value this_arg, napi_value result) {
  if (this_arg == nullptr || result == nullptr) return;

  napi_value ondone = nullptr;
  napi_valuetype type = napi_undefined;
  if (napi_get_named_property(env, this_arg, "ondone", &ondone) != napi_ok ||
      ondone == nullptr ||
      napi_typeof(env, ondone, &type) != napi_ok ||
      type != napi_function) {
    return;
  }

  napi_value err = nullptr;
  napi_value value = nullptr;
  if (napi_get_element(env, result, 0, &err) != napi_ok || err == nullptr) err = Undefined(env);
  if (napi_get_element(env, result, 1, &value) != napi_ok || value == nullptr) value = Undefined(env);

  auto* task = new CryptoOnDoneTask();
  task->env = env;
  if (napi_create_reference(env, this_arg, 1, &task->this_arg_ref) != napi_ok ||
      napi_create_reference(env, ondone, 1, &task->ondone_ref) != napi_ok ||
      napi_create_reference(env, err, 1, &task->err_ref) != napi_ok ||
      napi_create_reference(env, value, 1, &task->value_ref) != napi_ok ||
      UbiRuntimePlatformEnqueueTask(
          env, RunCryptoOnDoneTask, task, CleanupCryptoOnDoneTask, kUbiRuntimePlatformTaskRefed) != napi_ok) {
    CleanupCryptoOnDoneTask(env, task);
    napi_value argv[2] = {err, value};
    napi_value ignored = nullptr;
    (void)UbiAsyncWrapMakeCallback(
        env, 0, this_arg, this_arg, ondone, 2, argv, &ignored, kUbiMakeCallbackNone);
  }
}

bool ReadKeyEncodingSelection(napi_env env,
                              JobWrap* wrap,
                              size_t index,
                              KeyEncodingSelection* out,
                              std::string* error_code,
                              std::string* error_message) {
  if (wrap == nullptr || out == nullptr || error_code == nullptr || error_message == nullptr) return false;
  if (wrap->args.size() < index + 6) {
    *error_code = "ERR_CRYPTO_OPERATION_FAILED";
    *error_message = "Invalid key encoding arguments";
    return false;
  }

  napi_value pub_format_v = GetRefValue(env, wrap->args[index + 0]);
  napi_value pub_type_v = GetRefValue(env, wrap->args[index + 1]);
  napi_value priv_format_v = GetRefValue(env, wrap->args[index + 2]);
  napi_value priv_type_v = GetRefValue(env, wrap->args[index + 3]);
  napi_value cipher_v = GetRefValue(env, wrap->args[index + 4]);
  napi_value passphrase_v = GetRefValue(env, wrap->args[index + 5]);

  out->has_public_encoding = !IsNullOrUndefinedValue(env, pub_format_v);
  if (out->has_public_encoding) {
    out->public_format = GetInt32Value(env, pub_format_v, kKeyFormatPEM);
    out->public_type = GetInt32Value(env, pub_type_v, kKeyEncodingSPKI);
  }

  out->has_private_encoding = !IsNullOrUndefinedValue(env, priv_format_v);
  if (out->has_private_encoding) {
    out->private_format = GetInt32Value(env, priv_format_v, kKeyFormatPEM);
    out->private_type = GetInt32Value(env, priv_type_v, kKeyEncodingPKCS8);
    if (!IsNullOrUndefinedValue(env, cipher_v)) out->private_cipher = GetStringValue(env, cipher_v);
    if (!IsNullOrUndefinedValue(env, passphrase_v)) {
      out->private_passphrase = ValueToBytes(env, passphrase_v);
      out->private_passphrase_provided = true;
    }
  }
  return true;
}

std::vector<uint8_t> ExportPublicDerSpki(EVP_PKEY* pkey) {
  std::vector<uint8_t> out;
  BIO* bio = BIO_new(BIO_s_mem());
  if (bio == nullptr) return out;
  if (i2d_PUBKEY_bio(bio, pkey) == 1) out = BytesFromBio(bio);
  BIO_free(bio);
  return out;
}

std::vector<uint8_t> ExportPrivateDerPkcs8(EVP_PKEY* pkey) {
  std::vector<uint8_t> out;
  BIO* bio = BIO_new(BIO_s_mem());
  if (bio == nullptr) return out;
  if (i2d_PKCS8PrivateKey_bio(bio, pkey, nullptr, nullptr, 0, nullptr, nullptr) == 1) {
    out = BytesFromBio(bio);
  }
  BIO_free(bio);
  return out;
}

napi_value CreateKeyObjectHandleValue(napi_env env, int32_t key_type, const std::vector<uint8_t>& key_data) {
  napi_value binding = GetBinding(env);
  if (binding == nullptr) return Undefined(env);
  napi_value ctor = nullptr;
  if (napi_get_named_property(env, binding, "KeyObjectHandle", &ctor) != napi_ok || ctor == nullptr) {
    return Undefined(env);
  }
  napi_valuetype ctor_type = napi_undefined;
  if (napi_typeof(env, ctor, &ctor_type) != napi_ok || ctor_type != napi_function) return Undefined(env);

  napi_value handle = nullptr;
  if (napi_new_instance(env, ctor, 0, nullptr, &handle) != napi_ok || handle == nullptr) return Undefined(env);

  napi_value init_fn = nullptr;
  if (napi_get_named_property(env, handle, "init", &init_fn) != napi_ok || init_fn == nullptr) return Undefined(env);
  napi_valuetype init_type = napi_undefined;
  if (napi_typeof(env, init_fn, &init_type) != napi_ok || init_type != napi_function) return Undefined(env);

  napi_value key_type_v = nullptr;
  napi_create_int32(env, key_type, &key_type_v);
  napi_value key_data_v = BytesToBuffer(env, key_data);
  napi_value argv[2] = {key_type_v != nullptr ? key_type_v : Undefined(env),
                        key_data_v != nullptr ? key_data_v : Undefined(env)};
  napi_value ignored = nullptr;
  napi_call_function(env, handle, init_fn, 2, argv, &ignored);
  return handle;
}

bool ExportRsaPublic(EVP_PKEY* pkey,
                     BIO* bio,
                     int32_t format,
                     int32_t type,
                     std::string* error_code,
                     std::string* error_message) {
  if (pkey == nullptr || bio == nullptr) return false;
  if (type == kKeyEncodingPKCS1) {
    RSA* rsa = EVP_PKEY_get1_RSA(pkey);
    if (rsa == nullptr) {
      *error_code = "ERR_CRYPTO_OPERATION_FAILED";
      *error_message = "RSA key export failed";
      return false;
    }
    const int ok = (format == kKeyFormatPEM) ? PEM_write_bio_RSAPublicKey(bio, rsa) : i2d_RSAPublicKey_bio(bio, rsa);
    RSA_free(rsa);
    if (ok != 1) {
      *error_code = "ERR_CRYPTO_OPERATION_FAILED";
      *error_message = "RSA key export failed";
      return false;
    }
    return true;
  }
  const int ok = (format == kKeyFormatPEM) ? PEM_write_bio_PUBKEY(bio, pkey) : i2d_PUBKEY_bio(bio, pkey);
  if (ok != 1) {
    *error_code = "ERR_CRYPTO_OPERATION_FAILED";
    *error_message = "Public key export failed";
    return false;
  }
  return true;
}

bool ExportRsaPrivate(EVP_PKEY* pkey,
                      BIO* bio,
                      int32_t format,
                      int32_t type,
                      const EVP_CIPHER* cipher,
                      const std::vector<uint8_t>& passphrase,
                      bool passphrase_provided,
                      std::string* error_code,
                      std::string* error_message) {
  if (pkey == nullptr || bio == nullptr) return false;
  unsigned char* pass_ptr = const_cast<unsigned char*>(
      passphrase_provided ? (passphrase.empty() ? reinterpret_cast<const unsigned char*>("") : passphrase.data())
                          : nullptr);
  int pass_len = passphrase_provided ? static_cast<int>(passphrase.size()) : 0;

  if (format == kKeyFormatPEM) {
    if (type == kKeyEncodingPKCS1) {
      RSA* rsa = EVP_PKEY_get1_RSA(pkey);
      if (rsa == nullptr) {
        *error_code = "ERR_CRYPTO_OPERATION_FAILED";
        *error_message = "RSA private key export failed";
        return false;
      }
      const int ok = PEM_write_bio_RSAPrivateKey(
          bio, rsa, cipher, pass_ptr, pass_len, nullptr, nullptr);
      RSA_free(rsa);
      if (ok != 1) {
        *error_code = "ERR_CRYPTO_OPERATION_FAILED";
        *error_message = "RSA private key export failed";
        return false;
      }
      return true;
    }
    const int ok = PEM_write_bio_PKCS8PrivateKey(
        bio, pkey, cipher, reinterpret_cast<char*>(pass_ptr), pass_len, nullptr, nullptr);
    if (ok != 1) {
      *error_code = "ERR_CRYPTO_OPERATION_FAILED";
      *error_message = "PKCS8 private key export failed";
      return false;
    }
    return true;
  }

  if (type == kKeyEncodingPKCS1) {
    if (cipher != nullptr || passphrase_provided) {
      *error_code = "ERR_CRYPTO_INCOMPATIBLE_KEY_OPTIONS";
      *error_message = "PKCS1 DER does not support encryption";
      return false;
    }
    RSA* rsa = EVP_PKEY_get1_RSA(pkey);
    if (rsa == nullptr || i2d_RSAPrivateKey_bio(bio, rsa) != 1) {
      if (rsa != nullptr) RSA_free(rsa);
      *error_code = "ERR_CRYPTO_OPERATION_FAILED";
      *error_message = "RSA private key export failed";
      return false;
    }
    RSA_free(rsa);
    return true;
  }

  const int ok = i2d_PKCS8PrivateKey_bio(
      bio, pkey, cipher, reinterpret_cast<char*>(pass_ptr), pass_len, nullptr, nullptr);
  if (ok != 1) {
    *error_code = "ERR_CRYPTO_OPERATION_FAILED";
    *error_message = "PKCS8 private key export failed";
    return false;
  }
  return true;
}

bool ExportEcPrivate(EVP_PKEY* pkey,
                     BIO* bio,
                     int32_t format,
                     int32_t type,
                     const EVP_CIPHER* cipher,
                     const std::vector<uint8_t>& passphrase,
                     bool passphrase_provided,
                     std::string* error_code,
                     std::string* error_message) {
  if (pkey == nullptr || bio == nullptr) return false;
  unsigned char* pass_ptr = const_cast<unsigned char*>(
      passphrase_provided ? (passphrase.empty() ? reinterpret_cast<const unsigned char*>("") : passphrase.data())
                          : nullptr);
  int pass_len = passphrase_provided ? static_cast<int>(passphrase.size()) : 0;

  if (type == kKeyEncodingSEC1) {
    EC_KEY* ec = EVP_PKEY_get1_EC_KEY(pkey);
    if (ec == nullptr) {
      *error_code = "ERR_CRYPTO_OPERATION_FAILED";
      *error_message = "EC private key export failed";
      return false;
    }
    int ok = 0;
    if (format == kKeyFormatPEM) {
      ok = PEM_write_bio_ECPrivateKey(ec == nullptr ? nullptr : bio, ec, cipher, pass_ptr, pass_len, nullptr, nullptr);
    } else {
      if (cipher != nullptr || passphrase_provided) {
        EC_KEY_free(ec);
        *error_code = "ERR_CRYPTO_INCOMPATIBLE_KEY_OPTIONS";
        *error_message = "SEC1 DER does not support encryption";
        return false;
      }
      ok = i2d_ECPrivateKey_bio(bio, ec);
    }
    EC_KEY_free(ec);
    if (ok != 1) {
      *error_code = "ERR_CRYPTO_OPERATION_FAILED";
      *error_message = "EC private key export failed";
      return false;
    }
    return true;
  }

  if (format == kKeyFormatPEM) {
    const int ok = PEM_write_bio_PKCS8PrivateKey(
        bio, pkey, cipher, reinterpret_cast<char*>(pass_ptr), pass_len, nullptr, nullptr);
    if (ok != 1) {
      *error_code = "ERR_CRYPTO_OPERATION_FAILED";
      *error_message = "PKCS8 private key export failed";
      return false;
    }
    return true;
  }

  const int ok = i2d_PKCS8PrivateKey_bio(
      bio, pkey, cipher, reinterpret_cast<char*>(pass_ptr), pass_len, nullptr, nullptr);
  if (ok != 1) {
    *error_code = "ERR_CRYPTO_OPERATION_FAILED";
    *error_message = "PKCS8 private key export failed";
    return false;
  }
  return true;
}

napi_value ExportJwkPublic(napi_env env,
                           EVP_PKEY* pkey,
                           std::string* error_code,
                           std::string* error_message,
                           const std::string& curve_name_hint) {
  napi_value jwk = nullptr;
  if (napi_create_object(env, &jwk) != napi_ok || jwk == nullptr) return Undefined(env);

  const int base = EVP_PKEY_base_id(pkey);
  if (base == EVP_PKEY_RSA || base == EVP_PKEY_RSA_PSS) {
    BIGNUM* n = nullptr;
    BIGNUM* e = nullptr;
    if (EVP_PKEY_get_bn_param(pkey, OSSL_PKEY_PARAM_RSA_N, &n) != 1 ||
        EVP_PKEY_get_bn_param(pkey, OSSL_PKEY_PARAM_RSA_E, &e) != 1 ||
        n == nullptr || e == nullptr) {
      if (n != nullptr) BN_free(n);
      if (e != nullptr) BN_free(e);
      *error_code = "ERR_CRYPTO_OPERATION_FAILED";
      *error_message = "Failed to export RSA key as JWK";
      return nullptr;
    }
    SetObjectString(env, jwk, "kty", "RSA");
    SetObjectString(env, jwk, "n", Base64UrlEncode(BigNumToBytes(n)));
    SetObjectString(env, jwk, "e", Base64UrlEncode(BigNumToBytes(e)));
    BN_free(n);
    BN_free(e);
    return jwk;
  }

  if (base == EVP_PKEY_EC) {
    int nid = NID_undef;
    std::string curve_name = curve_name_hint;
    char group_name[80];
    size_t group_name_len = 0;
    if (EVP_PKEY_get_utf8_string_param(pkey, OSSL_PKEY_PARAM_GROUP_NAME, group_name, sizeof(group_name),
                                       &group_name_len) == 1 &&
        group_name_len > 0) {
      curve_name.assign(group_name, group_name_len);
      nid = CurveNidFromName(curve_name);
    }
    if (nid == NID_undef) {
      EC_KEY* ec = EVP_PKEY_get1_EC_KEY(pkey);
      if (ec != nullptr) {
        const EC_GROUP* group = EC_KEY_get0_group(ec);
        if (group != nullptr) nid = EC_GROUP_get_curve_name(group);
        EC_KEY_free(ec);
      }
    }
    const std::string jwk_curve = JwkCurveFromNid(nid);
    if (jwk_curve.empty()) {
      *error_code = "ERR_CRYPTO_JWK_UNSUPPORTED_CURVE";
      *error_message = "Unsupported JWK EC curve: " + (curve_name.empty() ? "unknown" : curve_name) + ".";
      return nullptr;
    }

    BIGNUM* x = nullptr;
    BIGNUM* y = nullptr;
    if (EVP_PKEY_get_bn_param(pkey, OSSL_PKEY_PARAM_EC_PUB_X, &x) != 1 ||
        EVP_PKEY_get_bn_param(pkey, OSSL_PKEY_PARAM_EC_PUB_Y, &y) != 1 ||
        x == nullptr || y == nullptr) {
      if (x != nullptr) BN_free(x);
      if (y != nullptr) BN_free(y);
      *error_code = "ERR_CRYPTO_OPERATION_FAILED";
      *error_message = "Failed to export EC key as JWK";
      return nullptr;
    }
    SetObjectString(env, jwk, "kty", "EC");
    SetObjectString(env, jwk, "crv", jwk_curve);
    SetObjectString(env, jwk, "x", Base64UrlEncode(BigNumToBytes(x)));
    SetObjectString(env, jwk, "y", Base64UrlEncode(BigNumToBytes(y)));
    BN_free(x);
    BN_free(y);
    return jwk;
  }

  *error_code = "ERR_CRYPTO_JWK_UNSUPPORTED_KEY_TYPE";
  *error_message = "Unsupported JWK Key Type.";
  return nullptr;
}

napi_value ExportJwkPrivate(napi_env env,
                            EVP_PKEY* pkey,
                            std::string* error_code,
                            std::string* error_message,
                            const std::string& curve_name_hint) {
  napi_value jwk = ExportJwkPublic(env, pkey, error_code, error_message, curve_name_hint);
  if (jwk == nullptr) return nullptr;

  const int base = EVP_PKEY_base_id(pkey);
  if (base == EVP_PKEY_RSA || base == EVP_PKEY_RSA_PSS) {
    BIGNUM* d = nullptr;
    BIGNUM* p = nullptr;
    BIGNUM* q = nullptr;
    BIGNUM* dp = nullptr;
    BIGNUM* dq = nullptr;
    BIGNUM* qi = nullptr;
    bool ok = EVP_PKEY_get_bn_param(pkey, OSSL_PKEY_PARAM_RSA_D, &d) == 1 &&
              EVP_PKEY_get_bn_param(pkey, OSSL_PKEY_PARAM_RSA_FACTOR1, &p) == 1 &&
              EVP_PKEY_get_bn_param(pkey, OSSL_PKEY_PARAM_RSA_FACTOR2, &q) == 1 &&
              EVP_PKEY_get_bn_param(pkey, OSSL_PKEY_PARAM_RSA_EXPONENT1, &dp) == 1 &&
              EVP_PKEY_get_bn_param(pkey, OSSL_PKEY_PARAM_RSA_EXPONENT2, &dq) == 1 &&
              EVP_PKEY_get_bn_param(pkey, OSSL_PKEY_PARAM_RSA_COEFFICIENT1, &qi) == 1 &&
              d != nullptr && p != nullptr && q != nullptr && dp != nullptr && dq != nullptr && qi != nullptr;
    if (!ok) {
      if (d != nullptr) BN_free(d);
      if (p != nullptr) BN_free(p);
      if (q != nullptr) BN_free(q);
      if (dp != nullptr) BN_free(dp);
      if (dq != nullptr) BN_free(dq);
      if (qi != nullptr) BN_free(qi);
      *error_code = "ERR_CRYPTO_OPERATION_FAILED";
      *error_message = "Failed to export RSA private key as JWK";
      return nullptr;
    }
    SetObjectString(env, jwk, "d", Base64UrlEncode(BigNumToBytes(d)));
    SetObjectString(env, jwk, "p", Base64UrlEncode(BigNumToBytes(p)));
    SetObjectString(env, jwk, "q", Base64UrlEncode(BigNumToBytes(q)));
    SetObjectString(env, jwk, "dp", Base64UrlEncode(BigNumToBytes(dp)));
    SetObjectString(env, jwk, "dq", Base64UrlEncode(BigNumToBytes(dq)));
    SetObjectString(env, jwk, "qi", Base64UrlEncode(BigNumToBytes(qi)));
    BN_free(d);
    BN_free(p);
    BN_free(q);
    BN_free(dp);
    BN_free(dq);
    BN_free(qi);
    return jwk;
  }

  if (base == EVP_PKEY_EC) {
    BIGNUM* d = nullptr;
    if (EVP_PKEY_get_bn_param(pkey, OSSL_PKEY_PARAM_PRIV_KEY, &d) != 1 || d == nullptr) {
      if (d != nullptr) BN_free(d);
      *error_code = "ERR_CRYPTO_OPERATION_FAILED";
      *error_message = "Failed to export EC private key as JWK";
      return nullptr;
    }
    SetObjectString(env, jwk, "d", Base64UrlEncode(BigNumToBytes(d)));
    BN_free(d);
    return jwk;
  }

  *error_code = "ERR_CRYPTO_JWK_UNSUPPORTED_KEY_TYPE";
  *error_message = "Unsupported JWK Key Type.";
  return nullptr;
}

bool ExportPublicKeyValue(napi_env env,
                          EVP_PKEY* pkey,
                          const KeyEncodingSelection& encoding,
                          const std::string& curve_name_hint,
                          napi_value* out_value,
                          std::string* error_code,
                          std::string* error_message) {
  if (!encoding.has_public_encoding) {
    const std::vector<uint8_t> der_spki = ExportPublicDerSpki(pkey);
    *out_value = CreateKeyObjectHandleValue(env, kKeyTypePublic, der_spki);
    if (*out_value == nullptr || IsUndefined(env, *out_value)) {
      *error_code = "ERR_CRYPTO_OPERATION_FAILED";
      *error_message = "Failed to create public key object";
      return false;
    }
    return true;
  }

  if (encoding.public_format == kKeyFormatJWK) {
    *out_value = ExportJwkPublic(env, pkey, error_code, error_message, curve_name_hint);
    return *out_value != nullptr;
  }

  BIO* bio = BIO_new(BIO_s_mem());
  if (bio == nullptr) {
    *error_code = "ERR_CRYPTO_OPERATION_FAILED";
    *error_message = "Failed to allocate BIO";
    return false;
  }
  const bool ok = ExportRsaPublic(pkey, bio, encoding.public_format, encoding.public_type, error_code, error_message);
  if (!ok) {
    BIO_free(bio);
    return false;
  }
  const std::vector<uint8_t> bytes = BytesFromBio(bio);
  BIO_free(bio);
  if (encoding.public_format == kKeyFormatPEM) {
    std::string text(bytes.begin(), bytes.end());
    napi_create_string_utf8(env, text.c_str(), text.size(), out_value);
  } else {
    *out_value = BytesToBuffer(env, bytes);
  }
  return *out_value != nullptr && !IsUndefined(env, *out_value);
}

bool ExportPrivateKeyValue(napi_env env,
                           EVP_PKEY* pkey,
                           const KeyEncodingSelection& encoding,
                           const std::string& curve_name_hint,
                           napi_value* out_value,
                           std::string* error_code,
                           std::string* error_message) {
  if (!encoding.has_private_encoding) {
    const std::vector<uint8_t> der_pkcs8 = ExportPrivateDerPkcs8(pkey);
    *out_value = CreateKeyObjectHandleValue(env, kKeyTypePrivate, der_pkcs8);
    if (*out_value == nullptr || IsUndefined(env, *out_value)) {
      *error_code = "ERR_CRYPTO_OPERATION_FAILED";
      *error_message = "Failed to create private key object";
      return false;
    }
    return true;
  }

  if (encoding.private_format == kKeyFormatJWK) {
    *out_value = ExportJwkPrivate(env, pkey, error_code, error_message, curve_name_hint);
    return *out_value != nullptr;
  }

  const EVP_CIPHER* cipher = nullptr;
  if (!encoding.private_cipher.empty()) {
    cipher = EVP_get_cipherbyname(encoding.private_cipher.c_str());
    if (cipher == nullptr) {
      *error_code = "ERR_CRYPTO_UNKNOWN_CIPHER";
      *error_message = "Unknown cipher";
      return false;
    }
  }

  BIO* bio = BIO_new(BIO_s_mem());
  if (bio == nullptr) {
    *error_code = "ERR_CRYPTO_OPERATION_FAILED";
    *error_message = "Failed to allocate BIO";
    return false;
  }

  const int base = EVP_PKEY_base_id(pkey);
  bool ok = false;
  if (base == EVP_PKEY_RSA || base == EVP_PKEY_RSA_PSS) {
    ok = ExportRsaPrivate(pkey,
                          bio,
                          encoding.private_format,
                          encoding.private_type,
                          cipher,
                          encoding.private_passphrase,
                          encoding.private_passphrase_provided,
                          error_code,
                          error_message);
  } else if (base == EVP_PKEY_EC) {
    ok = ExportEcPrivate(pkey,
                         bio,
                         encoding.private_format,
                         encoding.private_type,
                         cipher,
                         encoding.private_passphrase,
                         encoding.private_passphrase_provided,
                         error_code,
                         error_message);
  } else {
    if (encoding.private_format == kKeyFormatPEM) {
      unsigned char* pass_ptr = const_cast<unsigned char*>(
          encoding.private_passphrase_provided
              ? (encoding.private_passphrase.empty() ? reinterpret_cast<const unsigned char*>("")
                                                     : encoding.private_passphrase.data())
              : nullptr);
      int pass_len = encoding.private_passphrase_provided ? static_cast<int>(encoding.private_passphrase.size()) : 0;
      ok = PEM_write_bio_PrivateKey(
               bio, pkey, cipher, pass_ptr, pass_len, nullptr, nullptr) == 1;
    } else {
      unsigned char* pass_ptr = const_cast<unsigned char*>(
          encoding.private_passphrase_provided
              ? (encoding.private_passphrase.empty() ? reinterpret_cast<const unsigned char*>("")
                                                     : encoding.private_passphrase.data())
              : nullptr);
      int pass_len = encoding.private_passphrase_provided ? static_cast<int>(encoding.private_passphrase.size()) : 0;
      ok = i2d_PKCS8PrivateKey_bio(
               bio, pkey, cipher, reinterpret_cast<char*>(pass_ptr), pass_len, nullptr, nullptr) == 1;
    }
    if (!ok) {
      *error_code = "ERR_CRYPTO_OPERATION_FAILED";
      *error_message = "Private key export failed";
    }
  }

  if (!ok) {
    BIO_free(bio);
    return false;
  }

  const std::vector<uint8_t> bytes = BytesFromBio(bio);
  BIO_free(bio);
  if (encoding.private_format == kKeyFormatPEM) {
    std::string text(bytes.begin(), bytes.end());
    napi_create_string_utf8(env, text.c_str(), text.size(), out_value);
  } else {
    *out_value = BytesToBuffer(env, bytes);
  }
  return *out_value != nullptr && !IsUndefined(env, *out_value);
}

EVP_PKEY* GenerateRsaKeyPairNative(int32_t variant,
                                   int32_t modulus_length,
                                   uint32_t public_exponent,
                                   const std::string& hash_algorithm,
                                   const std::string& mgf1_hash_algorithm,
                                   int32_t salt_length,
                                   std::string* error_code,
                                   std::string* error_message) {
  if (modulus_length <= 0) {
    *error_code = "ERR_INVALID_ARG_VALUE";
    *error_message = "Invalid modulusLength";
    return nullptr;
  }
  const bool rsa_pss = variant == kKeyVariantRSA_PSS;
  EVP_PKEY_CTX* ctx = EVP_PKEY_CTX_new_id(rsa_pss ? EVP_PKEY_RSA_PSS : EVP_PKEY_RSA, nullptr);
  if (ctx == nullptr) {
    *error_code = "ERR_CRYPTO_OPERATION_FAILED";
    *error_message = "Failed to allocate RSA keygen context";
    return nullptr;
  }
  if (EVP_PKEY_keygen_init(ctx) != 1 ||
      EVP_PKEY_CTX_set_rsa_keygen_bits(ctx, modulus_length) != 1) {
    EVP_PKEY_CTX_free(ctx);
    *error_code = "ERR_CRYPTO_OPERATION_FAILED";
    *error_message = "RSA key generation initialization failed";
    return nullptr;
  }

  BIGNUM* exp = BN_new();
  if (exp == nullptr || BN_set_word(exp, public_exponent) != 1 ||
      EVP_PKEY_CTX_set_rsa_keygen_pubexp(ctx, exp) != 1) {
    const unsigned long err = ERR_get_error();
    if (exp != nullptr) BN_free(exp);
    EVP_PKEY_CTX_free(ctx);
    *error_code = "ERR_CRYPTO_OPERATION_FAILED";
    const char* reason = err == 0 ? nullptr : ERR_reason_error_string(err);
    *error_message = (reason != nullptr && reason[0] != '\0') ? reason : "RSA public exponent configuration failed";
    return nullptr;
  }
  // Ownership transferred on success.
  exp = nullptr;

  if (rsa_pss) {
    const EVP_MD* hash_md = nullptr;
    const EVP_MD* mgf1_md = nullptr;
    if (!hash_algorithm.empty()) {
      hash_md = EVP_get_digestbyname(hash_algorithm.c_str());
      if (hash_md == nullptr || EVP_PKEY_CTX_set_rsa_pss_keygen_md(ctx, hash_md) != 1) {
        EVP_PKEY_CTX_free(ctx);
        *error_code = "ERR_CRYPTO_INVALID_DIGEST";
        *error_message = "Invalid digest";
        return nullptr;
      }
    }
    std::string mgf1_name = mgf1_hash_algorithm;
    if (mgf1_name.empty() && hash_md != nullptr) mgf1_name = hash_algorithm;
    if (!mgf1_name.empty()) {
      mgf1_md = EVP_get_digestbyname(mgf1_name.c_str());
      if (mgf1_md == nullptr || EVP_PKEY_CTX_set_rsa_pss_keygen_mgf1_md(ctx, mgf1_md) != 1) {
        EVP_PKEY_CTX_free(ctx);
        *error_code = "ERR_CRYPTO_INVALID_DIGEST";
        *error_message = "Invalid digest";
        return nullptr;
      }
    }
    int32_t effective_salt_length = salt_length;
    if (effective_salt_length < 0 && hash_md != nullptr) {
      const int digest_size = EVP_MD_get_size(hash_md);
      if (digest_size > 0) effective_salt_length = digest_size;
    }
    if (effective_salt_length >= 0 &&
        EVP_PKEY_CTX_set_rsa_pss_keygen_saltlen(ctx, effective_salt_length) != 1) {
      EVP_PKEY_CTX_free(ctx);
      *error_code = "ERR_CRYPTO_OPERATION_FAILED";
      *error_message = "RSA-PSS salt length configuration failed";
      return nullptr;
    }
  }

  EVP_PKEY* pkey = nullptr;
  if (EVP_PKEY_keygen(ctx, &pkey) != 1 || pkey == nullptr) {
    const unsigned long err = ERR_get_error();
    EVP_PKEY_CTX_free(ctx);
    *error_code = "ERR_CRYPTO_OPERATION_FAILED";
    const char* reason = err == 0 ? nullptr : ERR_reason_error_string(err);
    *error_message = (reason != nullptr && reason[0] != '\0') ? reason : "RSA key generation failed";
    return nullptr;
  }
  EVP_PKEY_CTX_free(ctx);
  return pkey;
}

EVP_PKEY* GenerateDsaKeyPairNative(int32_t modulus_length,
                                   int32_t divisor_length,
                                   std::string* error_code,
                                   std::string* error_message) {
  EVP_PKEY_CTX* param_ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_DSA, nullptr);
  if (param_ctx == nullptr) {
    *error_code = "ERR_CRYPTO_OPERATION_FAILED";
    *error_message = "Failed to allocate DSA paramgen context";
    return nullptr;
  }
  if (EVP_PKEY_paramgen_init(param_ctx) != 1 ||
      EVP_PKEY_CTX_set_dsa_paramgen_bits(param_ctx, modulus_length) != 1) {
    EVP_PKEY_CTX_free(param_ctx);
    *error_code = "ERR_CRYPTO_OPERATION_FAILED";
    *error_message = "DSA parameter generation initialization failed";
    return nullptr;
  }
  if (divisor_length > 0 && EVP_PKEY_CTX_set_dsa_paramgen_q_bits(param_ctx, divisor_length) != 1) {
    EVP_PKEY_CTX_free(param_ctx);
    *error_code = "ERR_CRYPTO_OPERATION_FAILED";
    *error_message = "DSA divisor length configuration failed";
    return nullptr;
  }

  EVP_PKEY* params = nullptr;
  if (EVP_PKEY_paramgen(param_ctx, &params) != 1 || params == nullptr) {
    EVP_PKEY_CTX_free(param_ctx);
    *error_code = "ERR_CRYPTO_OPERATION_FAILED";
    *error_message = "DSA parameter generation failed";
    return nullptr;
  }
  EVP_PKEY_CTX_free(param_ctx);

  EVP_PKEY_CTX* key_ctx = EVP_PKEY_CTX_new(params, nullptr);
  if (key_ctx == nullptr || EVP_PKEY_keygen_init(key_ctx) != 1) {
    if (key_ctx != nullptr) EVP_PKEY_CTX_free(key_ctx);
    EVP_PKEY_free(params);
    *error_code = "ERR_CRYPTO_OPERATION_FAILED";
    *error_message = "DSA key generation initialization failed";
    return nullptr;
  }
  EVP_PKEY* pkey = nullptr;
  if (EVP_PKEY_keygen(key_ctx, &pkey) != 1 || pkey == nullptr) {
    EVP_PKEY_CTX_free(key_ctx);
    EVP_PKEY_free(params);
    *error_code = "ERR_CRYPTO_OPERATION_FAILED";
    *error_message = "DSA key generation failed";
    return nullptr;
  }
  EVP_PKEY_CTX_free(key_ctx);
  EVP_PKEY_free(params);
  return pkey;
}

EVP_PKEY* WrapDhAsPkey(DH* dh, std::string* error_code, std::string* error_message) {
  if (dh == nullptr) {
    *error_code = "ERR_CRYPTO_OPERATION_FAILED";
    *error_message = "DH initialization failed";
    return nullptr;
  }
  if (DH_generate_key(dh) != 1) {
    DH_free(dh);
    *error_code = "ERR_CRYPTO_OPERATION_FAILED";
    *error_message = "DH key generation failed";
    return nullptr;
  }
  EVP_PKEY* pkey = EVP_PKEY_new();
  if (pkey == nullptr || EVP_PKEY_assign_DH(pkey, dh) != 1) {
    if (pkey != nullptr) EVP_PKEY_free(pkey);
    DH_free(dh);
    *error_code = "ERR_CRYPTO_OPERATION_FAILED";
    *error_message = "DH key wrapping failed";
    return nullptr;
  }
  return pkey;
}

EVP_PKEY* GenerateDhKeyPairFromGroupNative(const std::string& group_name,
                                           std::string* error_code,
                                           std::string* error_message) {
  int32_t verify_error = 0;
  DH* dh = CreateDhFromGroupName(group_name, &verify_error);
  if (dh == nullptr) {
    *error_code = "ERR_CRYPTO_UNKNOWN_DH_GROUP";
    *error_message = "Unknown DH group";
    return nullptr;
  }
  return WrapDhAsPkey(dh, error_code, error_message);
}

EVP_PKEY* GenerateDhKeyPairFromPrimeNative(const std::vector<uint8_t>& prime,
                                           int32_t generator,
                                           std::string* error_code,
                                           std::string* error_message) {
  int32_t verify_error = 0;
  DH* dh = CreateDhFromPrimeAndGenerator(prime, generator, {}, &verify_error);
  if (dh == nullptr) {
    *error_code = "ERR_CRYPTO_OPERATION_FAILED";
    *error_message = "DH parameter initialization failed";
    return nullptr;
  }
  return WrapDhAsPkey(dh, error_code, error_message);
}

EVP_PKEY* GenerateDhKeyPairFromPrimeLengthNative(int32_t prime_length,
                                                 int32_t generator,
                                                 std::string* error_code,
                                                 std::string* error_message) {
  int32_t verify_error = 0;
  DH* dh = CreateDhFromSize(prime_length, generator, {}, &verify_error);
  if (dh == nullptr) {
    *error_code = "ERR_CRYPTO_OPERATION_FAILED";
    *error_message = "DH parameter generation failed";
    return nullptr;
  }
  return WrapDhAsPkey(dh, error_code, error_message);
}

EVP_PKEY* GenerateEcKeyPairNative(const std::string& named_curve,
                                  int32_t param_encoding,
                                  std::string* error_code,
                                  std::string* error_message) {
  const int nid = CurveNidFromName(named_curve);
  if (nid == NID_undef) {
    *error_code = "ERR_CRYPTO_INVALID_CURVE";
    *error_message = "Invalid EC curve name";
    return nullptr;
  }
  EVP_PKEY_CTX* ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_EC, nullptr);
  if (ctx == nullptr || EVP_PKEY_keygen_init(ctx) != 1 ||
      EVP_PKEY_CTX_set_ec_paramgen_curve_nid(ctx, nid) != 1 ||
      EVP_PKEY_CTX_set_ec_param_enc(ctx, param_encoding) != 1) {
    if (ctx != nullptr) EVP_PKEY_CTX_free(ctx);
    *error_code = "ERR_CRYPTO_OPERATION_FAILED";
    *error_message = "EC key generation initialization failed";
    return nullptr;
  }
  EVP_PKEY* pkey = nullptr;
  if (EVP_PKEY_keygen(ctx, &pkey) != 1 || pkey == nullptr) {
    EVP_PKEY_CTX_free(ctx);
    *error_code = "ERR_CRYPTO_OPERATION_FAILED";
    *error_message = "EC key generation failed";
    return nullptr;
  }
  EVP_PKEY_CTX_free(ctx);
  return pkey;
}

EVP_PKEY* GenerateNidKeyPairNative(int32_t nid, std::string* error_code, std::string* error_message) {
  EVP_PKEY_CTX* ctx = EVP_PKEY_CTX_new_id(nid, nullptr);
  if (ctx == nullptr || EVP_PKEY_keygen_init(ctx) != 1) {
    if (ctx != nullptr) EVP_PKEY_CTX_free(ctx);
    *error_code = "ERR_CRYPTO_OPERATION_FAILED";
    *error_message = "Unsupported key type";
    return nullptr;
  }
  EVP_PKEY* pkey = nullptr;
  if (EVP_PKEY_keygen(ctx, &pkey) != 1 || pkey == nullptr) {
    EVP_PKEY_CTX_free(ctx);
    *error_code = "ERR_CRYPTO_OPERATION_FAILED";
    *error_message = "Key generation failed";
    return nullptr;
  }
  EVP_PKEY_CTX_free(ctx);
  return pkey;
}

napi_value BuildGeneratedKeyPairResult(napi_env env,
                                       EVP_PKEY* pkey,
                                       const KeyEncodingSelection& encoding,
                                       const std::string& curve_name_hint) {
  std::string error_code;
  std::string error_message;
  napi_value public_key = nullptr;
  napi_value private_key = nullptr;
  if (!ExportPublicKeyValue(env, pkey, encoding, curve_name_hint, &public_key, &error_code, &error_message)) {
    if (error_code.empty()) error_code = "ERR_CRYPTO_OPERATION_FAILED";
    if (error_message.empty()) error_message = "Public key export failed";
    return BuildJobErrorResult(env, error_code.c_str(), error_message);
  }
  if (!ExportPrivateKeyValue(env, pkey, encoding, curve_name_hint, &private_key, &error_code, &error_message)) {
    if (error_code.empty()) error_code = "ERR_CRYPTO_OPERATION_FAILED";
    if (error_message.empty()) error_message = "Private key export failed";
    return BuildJobErrorResult(env, error_code.c_str(), error_message);
  }

  napi_value keys = nullptr;
  napi_create_array_with_length(env, 2, &keys);
  if (keys == nullptr) return BuildJobErrorResult(env, "ERR_CRYPTO_OPERATION_FAILED", "Failed to allocate keys");
  napi_set_element(env, keys, 0, public_key != nullptr ? public_key : Undefined(env));
  napi_set_element(env, keys, 1, private_key != nullptr ? private_key : Undefined(env));
  return BuildJobResult(env, nullptr, keys);
}

napi_value FinalizeJobRunResult(napi_env env, napi_value this_arg, JobWrap* wrap, napi_value result) {
  if (wrap != nullptr && wrap->mode == kCryptoJobAsync) {
    InvokeJobOnDone(env, this_arg, result);
    return Undefined(env);
  }
  return result;
}

napi_value RsaKeyPairGenJobRun(napi_env env, napi_callback_info info) {
  napi_value this_arg = nullptr;
  size_t argc = 0;
  napi_get_cb_info(env, info, &argc, nullptr, &this_arg, nullptr);
  JobWrap* wrap = UnwrapJob(env, this_arg);
  if (wrap == nullptr) {
    return BuildJobErrorResult(env, "ERR_CRYPTO_OPERATION_FAILED", "Invalid RSA keygen arguments");
  }
  if (wrap->args.size() < 9) {
    napi_value result = BuildJobErrorResult(env, "ERR_CRYPTO_OPERATION_FAILED", "Invalid RSA keygen arguments");
    return FinalizeJobRunResult(env, this_arg, wrap, result);
  }

  const int32_t variant = GetInt32Value(env, GetRefValue(env, wrap->args[0]), kKeyVariantRSA_SSA_PKCS1_v1_5);
  const int32_t modulus_length = GetInt32Value(env, GetRefValue(env, wrap->args[1]), 0);
  const uint32_t public_exponent = static_cast<uint32_t>(GetInt32Value(env, GetRefValue(env, wrap->args[2]), 0x10001));

  std::string hash_algorithm;
  std::string mgf1_hash_algorithm;
  int32_t salt_length = -1;
  size_t encoding_index = 3;
  if (variant == kKeyVariantRSA_PSS && wrap->args.size() >= 12) {
    hash_algorithm = GetStringValue(env, GetRefValue(env, wrap->args[3]));
    mgf1_hash_algorithm = GetStringValue(env, GetRefValue(env, wrap->args[4]));
    salt_length = IsNullOrUndefinedValue(env, GetRefValue(env, wrap->args[5]))
                      ? -1
                      : GetInt32Value(env, GetRefValue(env, wrap->args[5]), -1);
    encoding_index = 6;
  }

  KeyEncodingSelection encoding;
  std::string error_code;
  std::string error_message;
  if (!ReadKeyEncodingSelection(env, wrap, encoding_index, &encoding, &error_code, &error_message)) {
    return FinalizeJobRunResult(env, this_arg, wrap, BuildJobErrorResult(env, error_code.c_str(), error_message));
  }

  EVP_PKEY* pkey = GenerateRsaKeyPairNative(
      variant, modulus_length, public_exponent, hash_algorithm, mgf1_hash_algorithm, salt_length, &error_code,
      &error_message);
  if (pkey == nullptr) {
    if (error_code.empty()) error_code = "ERR_CRYPTO_OPERATION_FAILED";
    if (error_message.empty()) error_message = "RSA key generation failed";
    return FinalizeJobRunResult(env, this_arg, wrap, BuildJobErrorResult(env, error_code.c_str(), error_message));
  }

  napi_value result = BuildGeneratedKeyPairResult(env, pkey, encoding, "");
  EVP_PKEY_free(pkey);
  return FinalizeJobRunResult(env, this_arg, wrap, result);
}

napi_value DsaKeyPairGenJobRun(napi_env env, napi_callback_info info) {
  napi_value this_arg = nullptr;
  size_t argc = 0;
  napi_get_cb_info(env, info, &argc, nullptr, &this_arg, nullptr);
  JobWrap* wrap = UnwrapJob(env, this_arg);
  if (wrap == nullptr) {
    return BuildJobErrorResult(env, "ERR_CRYPTO_OPERATION_FAILED", "Invalid DSA keygen arguments");
  }
  if (wrap->args.size() < 8) {
    napi_value result = BuildJobErrorResult(env, "ERR_CRYPTO_OPERATION_FAILED", "Invalid DSA keygen arguments");
    return FinalizeJobRunResult(env, this_arg, wrap, result);
  }

  const int32_t modulus_length = GetInt32Value(env, GetRefValue(env, wrap->args[0]), 0);
  const int32_t divisor_length = GetInt32Value(env, GetRefValue(env, wrap->args[1]), -1);
  KeyEncodingSelection encoding;
  std::string error_code;
  std::string error_message;
  if (!ReadKeyEncodingSelection(env, wrap, 2, &encoding, &error_code, &error_message)) {
    return FinalizeJobRunResult(env, this_arg, wrap, BuildJobErrorResult(env, error_code.c_str(), error_message));
  }

  EVP_PKEY* pkey = GenerateDsaKeyPairNative(modulus_length, divisor_length, &error_code, &error_message);
  if (pkey == nullptr) {
    if (error_code.empty()) error_code = "ERR_CRYPTO_OPERATION_FAILED";
    if (error_message.empty()) error_message = "DSA key generation failed";
    return FinalizeJobRunResult(env, this_arg, wrap, BuildJobErrorResult(env, error_code.c_str(), error_message));
  }

  napi_value result = BuildGeneratedKeyPairResult(env, pkey, encoding, "");
  EVP_PKEY_free(pkey);
  return FinalizeJobRunResult(env, this_arg, wrap, result);
}

napi_value DhKeyPairGenJobRun(napi_env env, napi_callback_info info) {
  napi_value this_arg = nullptr;
  size_t argc = 0;
  napi_get_cb_info(env, info, &argc, nullptr, &this_arg, nullptr);
  JobWrap* wrap = UnwrapJob(env, this_arg);
  if (wrap == nullptr) {
    return BuildJobErrorResult(env, "ERR_CRYPTO_OPERATION_FAILED", "Invalid DH keygen arguments");
  }
  if (wrap->args.size() < 7) {
    napi_value result = BuildJobErrorResult(env, "ERR_CRYPTO_OPERATION_FAILED", "Invalid DH keygen arguments");
    return FinalizeJobRunResult(env, this_arg, wrap, result);
  }

  napi_value source = GetRefValue(env, wrap->args[0]);
  napi_valuetype source_type = napi_undefined;
  if (source != nullptr) napi_typeof(env, source, &source_type);
  const bool group_mode = source_type == napi_string;
  const size_t encoding_index = group_mode ? 1 : 2;

  KeyEncodingSelection encoding;
  std::string error_code;
  std::string error_message;
  if (!ReadKeyEncodingSelection(env, wrap, encoding_index, &encoding, &error_code, &error_message)) {
    return FinalizeJobRunResult(env, this_arg, wrap, BuildJobErrorResult(env, error_code.c_str(), error_message));
  }

  EVP_PKEY* pkey = nullptr;
  if (group_mode) {
    const std::string group_name = GetStringValue(env, source);
    pkey = GenerateDhKeyPairFromGroupNative(group_name, &error_code, &error_message);
  } else if (source_type == napi_number) {
    const int32_t prime_length = GetInt32Value(env, source, 0);
    const int32_t generator = GetInt32Value(env, GetRefValue(env, wrap->args[1]), 2);
    pkey = GenerateDhKeyPairFromPrimeLengthNative(prime_length, generator, &error_code, &error_message);
  } else {
    const std::vector<uint8_t> prime = ValueToBytes(env, source);
    const int32_t generator = GetInt32Value(env, GetRefValue(env, wrap->args[1]), 2);
    pkey = GenerateDhKeyPairFromPrimeNative(prime, generator, &error_code, &error_message);
  }

  if (pkey == nullptr) {
    if (error_code.empty()) error_code = "ERR_CRYPTO_OPERATION_FAILED";
    if (error_message.empty()) error_message = "DH key generation failed";
    return FinalizeJobRunResult(env, this_arg, wrap, BuildJobErrorResult(env, error_code.c_str(), error_message));
  }

  napi_value result = BuildGeneratedKeyPairResult(env, pkey, encoding, "");
  EVP_PKEY_free(pkey);
  return FinalizeJobRunResult(env, this_arg, wrap, result);
}

napi_value EcKeyPairGenJobRun(napi_env env, napi_callback_info info) {
  napi_value this_arg = nullptr;
  size_t argc = 0;
  napi_get_cb_info(env, info, &argc, nullptr, &this_arg, nullptr);
  JobWrap* wrap = UnwrapJob(env, this_arg);
  if (wrap == nullptr) {
    return BuildJobErrorResult(env, "ERR_CRYPTO_OPERATION_FAILED", "Invalid EC keygen arguments");
  }
  if (wrap->args.size() < 8) {
    napi_value result = BuildJobErrorResult(env, "ERR_CRYPTO_OPERATION_FAILED", "Invalid EC keygen arguments");
    return FinalizeJobRunResult(env, this_arg, wrap, result);
  }
  const std::string named_curve = GetStringValue(env, GetRefValue(env, wrap->args[0]));
  const int32_t param_encoding = GetInt32Value(env, GetRefValue(env, wrap->args[1]), OPENSSL_EC_NAMED_CURVE);

  KeyEncodingSelection encoding;
  std::string error_code;
  std::string error_message;
  if (!ReadKeyEncodingSelection(env, wrap, 2, &encoding, &error_code, &error_message)) {
    return FinalizeJobRunResult(env, this_arg, wrap, BuildJobErrorResult(env, error_code.c_str(), error_message));
  }

  EVP_PKEY* pkey = GenerateEcKeyPairNative(named_curve, param_encoding, &error_code, &error_message);
  if (pkey == nullptr) {
    if (error_code.empty()) error_code = "ERR_CRYPTO_OPERATION_FAILED";
    if (error_message.empty()) error_message = "EC key generation failed";
    return FinalizeJobRunResult(env, this_arg, wrap, BuildJobErrorResult(env, error_code.c_str(), error_message));
  }

  napi_value result = BuildGeneratedKeyPairResult(env, pkey, encoding, named_curve);
  EVP_PKEY_free(pkey);
  return FinalizeJobRunResult(env, this_arg, wrap, result);
}

napi_value NidKeyPairGenJobRun(napi_env env, napi_callback_info info) {
  napi_value this_arg = nullptr;
  size_t argc = 0;
  napi_get_cb_info(env, info, &argc, nullptr, &this_arg, nullptr);
  JobWrap* wrap = UnwrapJob(env, this_arg);
  if (wrap == nullptr) {
    return BuildJobErrorResult(env, "ERR_CRYPTO_OPERATION_FAILED", "Invalid keygen arguments");
  }
  if (wrap->args.size() < 7) {
    napi_value result = BuildJobErrorResult(env, "ERR_CRYPTO_OPERATION_FAILED", "Invalid keygen arguments");
    return FinalizeJobRunResult(env, this_arg, wrap, result);
  }

  const int32_t nid = GetInt32Value(env, GetRefValue(env, wrap->args[0]), 0);
  KeyEncodingSelection encoding;
  std::string error_code;
  std::string error_message;
  if (!ReadKeyEncodingSelection(env, wrap, 1, &encoding, &error_code, &error_message)) {
    return FinalizeJobRunResult(env, this_arg, wrap, BuildJobErrorResult(env, error_code.c_str(), error_message));
  }

  EVP_PKEY* pkey = GenerateNidKeyPairNative(nid, &error_code, &error_message);
  if (pkey == nullptr) {
    if (error_code.empty()) error_code = "ERR_CRYPTO_OPERATION_FAILED";
    if (error_message.empty()) error_message = "Key generation failed";
    return FinalizeJobRunResult(env, this_arg, wrap, BuildJobErrorResult(env, error_code.c_str(), error_message));
  }

  napi_value result = BuildGeneratedKeyPairResult(env, pkey, encoding, "");
  EVP_PKEY_free(pkey);
  return FinalizeJobRunResult(env, this_arg, wrap, result);
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
  if (wrap->mode == kCryptoJobAsync) {
    InvokeJobOnDone(env, this_arg, result);
    return Undefined(env);
  }
  return result;
}

napi_value PBKDF2JobRun(napi_env env, napi_callback_info info) {
  napi_value this_arg = nullptr;
  size_t argc = 0;
  napi_get_cb_info(env, info, &argc, nullptr, &this_arg, nullptr);
  JobWrap* wrap = UnwrapJob(env, this_arg);
  if (wrap == nullptr || wrap->args.size() < 5) return BuildJobResult(env, nullptr, Undefined(env));
  napi_value password = NormalizeToBufferObject(env, GetRefValue(env, wrap->args[0]));
  napi_value salt = NormalizeToBufferObject(env, GetRefValue(env, wrap->args[1]));
  napi_value iterations = GetRefValue(env, wrap->args[2]);
  napi_value keylen = GetRefValue(env, wrap->args[3]);
  napi_value digest = GetRefValue(env, wrap->args[4]);
  std::vector<napi_value> argsv = {
      password != nullptr ? password : Undefined(env),
      salt != nullptr ? salt : Undefined(env),
      iterations != nullptr ? iterations : Undefined(env),
      keylen != nullptr ? keylen : Undefined(env),
      digest != nullptr ? digest : Undefined(env),
  };
  napi_value result = RunSyncCall(env, this_arg, "pbkdf2Sync", argsv);
  if (wrap->mode == kCryptoJobAsync) {
    InvokeJobOnDone(env, this_arg, result);
    return Undefined(env);
  }
  return result;
}

napi_value ScryptJobRun(napi_env env, napi_callback_info info) {
  napi_value this_arg = nullptr;
  size_t argc = 0;
  napi_get_cb_info(env, info, &argc, nullptr, &this_arg, nullptr);
  JobWrap* wrap = UnwrapJob(env, this_arg);
  if (wrap == nullptr || wrap->args.size() < 7) return BuildJobResult(env, nullptr, Undefined(env));
  napi_value password = NormalizeToBufferObject(env, GetRefValue(env, wrap->args[0]));
  napi_value salt = NormalizeToBufferObject(env, GetRefValue(env, wrap->args[1]));
  napi_value N = GetRefValue(env, wrap->args[2]);
  napi_value r = GetRefValue(env, wrap->args[3]);
  napi_value p = GetRefValue(env, wrap->args[4]);
  napi_value maxmem = GetRefValue(env, wrap->args[5]);
  napi_value keylen = GetRefValue(env, wrap->args[6]);
  std::vector<napi_value> argsv = {password, salt, keylen, N, r, p, maxmem};
  napi_value result = RunSyncCall(env, this_arg, "scryptSync", argsv);
  if (wrap->mode == kCryptoJobAsync) {
    InvokeJobOnDone(env, this_arg, result);
    return Undefined(env);
  }
  return result;
}

napi_value HKDFJobRun(napi_env env, napi_callback_info info) {
  napi_value this_arg = nullptr;
  size_t argc = 0;
  napi_get_cb_info(env, info, &argc, nullptr, &this_arg, nullptr);
  JobWrap* wrap = UnwrapJob(env, this_arg);
  if (wrap == nullptr || wrap->args.size() < 5) return BuildJobResult(env, nullptr, Undefined(env));
  napi_value digest = GetRefValue(env, wrap->args[0]);
  napi_value key = GetRefValue(env, wrap->args[1]);
  napi_value salt = EnsureBufferValue(env, GetRefValue(env, wrap->args[2]));
  napi_value info_arg = EnsureBufferValue(env, GetRefValue(env, wrap->args[3]));
  napi_value keylen = GetRefValue(env, wrap->args[4]);
  std::vector<napi_value> argsv = {
      digest != nullptr ? digest : Undefined(env),
      key != nullptr ? key : Undefined(env),
      salt != nullptr ? salt : Undefined(env),
      info_arg != nullptr ? info_arg : Undefined(env),
      keylen != nullptr ? keylen : Undefined(env),
  };
  napi_value result = RunSyncCall(env, this_arg, "hkdfSync", argsv);
  if (result != nullptr && !IsUndefined(env, result)) {
    napi_value err = nullptr;
    napi_value value = nullptr;
    if (napi_get_element(env, result, 0, &err) == napi_ok &&
        napi_get_element(env, result, 1, &value) == napi_ok &&
        err != nullptr) {
      if (!IsUndefined(env, err) && !IsNullOrUndefinedValue(env, err)) {
        napi_value code = nullptr;
        size_t code_len = 0;
        if (napi_get_named_property(env, err, "code", &code) == napi_ok &&
            code != nullptr &&
            napi_get_value_string_utf8(env, code, nullptr, 0, &code_len) == napi_ok) {
          std::string code_text(code_len + 1, '\0');
          size_t copied = 0;
          if (napi_get_value_string_utf8(env, code, code_text.data(), code_text.size(), &copied) == napi_ok) {
            code_text.resize(copied);
            if (code_text == "ERR_INVALID_ARG_TYPE") {
              napi_value binding = GetBinding(env);
              if (!IsSupportedDigestName(env, binding, digest)) {
                napi_value mapped = nullptr;
                if (napi_create_string_utf8(env, "ERR_CRYPTO_INVALID_DIGEST", NAPI_AUTO_LENGTH, &mapped) == napi_ok &&
                    mapped != nullptr) {
                  napi_set_named_property(env, err, "code", mapped);
                }
              }
            }
          }
        }
      }
      if (value != nullptr && !IsUndefined(env, value)) {
        const napi_value converted = CopyAsArrayBuffer(env, value);
        result = BuildJobResult(env, err, converted);
      } else {
        result = BuildJobResult(env, err, value);
      }
    } else if (value != nullptr && !IsUndefined(env, value)) {
      const napi_value converted = CopyAsArrayBuffer(env, value);
      result = BuildJobResult(env, err, converted);
    }
  }
  if (wrap->mode == kCryptoJobAsync) {
    InvokeJobOnDone(env, this_arg, result);
    return Undefined(env);
  }
  return result;
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
  if (wrap->mode == kCryptoJobAsync) {
    InvokeJobOnDone(env, this_arg, result);
    return Undefined(env);
  }
  return result;
}

napi_value SignJobRun(napi_env env, napi_callback_info info) {
  napi_value this_arg = nullptr;
  size_t argc = 0;
  napi_get_cb_info(env, info, &argc, nullptr, &this_arg, nullptr);
  JobWrap* wrap = UnwrapJob(env, this_arg);
  if (wrap == nullptr) return BuildJobResult(env, nullptr, Undefined(env));
  if (wrap->args.size() < 11) {
    napi_value result = BuildJobErrorResult(env, "ERR_CRYPTO_OPERATION_FAILED", "Invalid sign job arguments");
    return FinalizeJobRunResult(env, this_arg, wrap, result);
  }

  int32_t job_mode = kSignJobModeSign;
  napi_value mode_v = GetRefValue(env, wrap->args[0]);
  if (mode_v != nullptr) napi_get_value_int32(env, mode_v, &job_mode);

  napi_value key = GetRefValue(env, wrap->args[1]);
  napi_value key_format = GetRefValue(env, wrap->args[2]);
  napi_value key_type = GetRefValue(env, wrap->args[3]);
  napi_value key_passphrase = GetRefValue(env, wrap->args[4]);
  napi_value data = EnsureBufferValue(env, GetRefValue(env, wrap->args[5]));
  napi_value algorithm = GetRefValue(env, wrap->args[6]);
  napi_value pss_salt_len = GetRefValue(env, wrap->args[7]);
  napi_value rsa_padding = GetRefValue(env, wrap->args[8]);
  napi_value dsa_sig_enc = GetRefValue(env, wrap->args[9]);
  napi_value context = GetRefValue(env, wrap->args[10]);

  napi_value result = nullptr;
  if (job_mode == kSignJobModeVerify) {
    napi_value signature =
        wrap->args.size() >= 12 ? EnsureBufferValue(env, GetRefValue(env, wrap->args[11])) : Undefined(env);
    std::vector<napi_value> call_args = {
        algorithm != nullptr ? algorithm : Undefined(env),
        data != nullptr ? data : Undefined(env),
        key != nullptr ? key : Undefined(env),
        key_format != nullptr ? key_format : Undefined(env),
        key_type != nullptr ? key_type : Undefined(env),
        key_passphrase != nullptr ? key_passphrase : Undefined(env),
        signature != nullptr ? signature : Undefined(env),
        rsa_padding != nullptr ? rsa_padding : Undefined(env),
        pss_salt_len != nullptr ? pss_salt_len : Undefined(env),
        dsa_sig_enc != nullptr ? dsa_sig_enc : Undefined(env),
        context != nullptr ? context : Undefined(env),
    };
    result = RunSyncCall(env, this_arg, "verifyOneShot", call_args);
  } else {
    std::vector<napi_value> call_args = {
        algorithm != nullptr ? algorithm : Undefined(env),
        data != nullptr ? data : Undefined(env),
        key != nullptr ? key : Undefined(env),
        key_format != nullptr ? key_format : Undefined(env),
        key_type != nullptr ? key_type : Undefined(env),
        key_passphrase != nullptr ? key_passphrase : Undefined(env),
        rsa_padding != nullptr ? rsa_padding : Undefined(env),
        pss_salt_len != nullptr ? pss_salt_len : Undefined(env),
        dsa_sig_enc != nullptr ? dsa_sig_enc : Undefined(env),
        context != nullptr ? context : Undefined(env),
    };
    result = RunSyncCall(env, this_arg, "signOneShot", call_args);
  }

  if (wrap->mode == kCryptoJobAsync) {
    InvokeJobOnDone(env, this_arg, result);
    return Undefined(env);
  }
  return result;
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
  (void)info;
#if OPENSSL_VERSION_NUMBER >= 0x10100000L
  if (CRYPTO_secure_malloc_initialized() != 1) {
    return Undefined(env);
  }
  napi_value out = nullptr;
  if (napi_create_double(env, static_cast<double>(CRYPTO_secure_used()), &out) != napi_ok || out == nullptr) {
    return Undefined(env);
  }
  return out;
#else
  return Undefined(env);
#endif
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
    if (CallBindingMethod(env, binding, "getSSLCiphers", 0, nullptr, &out) && out != nullptr) return out;
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

  EnsureCryptoCleanupHook(env);
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
      {"RSA_PKCS1_PADDING", RSA_PKCS1_PADDING},
      {"RSA_NO_PADDING", RSA_NO_PADDING},
      {"RSA_PKCS1_OAEP_PADDING", RSA_PKCS1_OAEP_PADDING},
      {"RSA_X931_PADDING", RSA_X931_PADDING},
      {"RSA_PKCS1_PSS_PADDING", RSA_PKCS1_PSS_PADDING},
      {"RSA_PSS_SALTLEN_DIGEST", RSA_PSS_SALTLEN_DIGEST},
      {"RSA_PSS_SALTLEN_MAX_SIGN", RSA_PSS_SALTLEN_MAX_SIGN},
      {"RSA_PSS_SALTLEN_AUTO", RSA_PSS_SALTLEN_AUTO},
      {"POINT_CONVERSION_COMPRESSED", POINT_CONVERSION_COMPRESSED},
      {"POINT_CONVERSION_UNCOMPRESSED", POINT_CONVERSION_UNCOMPRESSED},
      {"POINT_CONVERSION_HYBRID", POINT_CONVERSION_HYBRID},
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

  EnsureClass(env,
              out,
              "Sign",
              SignCtor,
              {
                  {"init", nullptr, SignVerifyInit, nullptr, nullptr, nullptr, napi_default, nullptr},
                  {"update", nullptr, SignVerifyUpdate, nullptr, nullptr, nullptr, napi_default, nullptr},
                  {"sign", nullptr, SignSign, nullptr, nullptr, nullptr, napi_default, nullptr},
              });

  EnsureClass(env,
              out,
              "Verify",
              VerifyCtor,
              {
                  {"init", nullptr, SignVerifyInit, nullptr, nullptr, nullptr, napi_default, nullptr},
                  {"update", nullptr, SignVerifyUpdate, nullptr, nullptr, nullptr, napi_default, nullptr},
                  {"verify", nullptr, VerifyVerify, nullptr, nullptr, nullptr, napi_default, nullptr},
              });

  EnsureClass(env,
              out,
              "CipherBase",
              CipherBaseCtor,
              {
                  {"update", nullptr, CipherBaseUpdate, nullptr, nullptr, nullptr, napi_default, nullptr},
                  {"final", nullptr, CipherBaseFinal, nullptr, nullptr, nullptr, napi_default, nullptr},
                  {"setAutoPadding", nullptr, CipherBaseSetAutoPadding, nullptr, nullptr, nullptr, napi_default,
                   nullptr},
                  {"setAAD", nullptr, CipherBaseSetAAD, nullptr, nullptr, nullptr, napi_default, nullptr},
                  {"getAuthTag", nullptr, CipherBaseGetAuthTag, nullptr, nullptr, nullptr, napi_default, nullptr},
                  {"setAuthTag", nullptr, CipherBaseSetAuthTag, nullptr, nullptr, nullptr, napi_default, nullptr},
              });

  EnsureClass(env,
              out,
              "SecureContext",
              SecureContextCtor,
              {
                  {"init", nullptr, SecureContextInit, nullptr, nullptr, nullptr, napi_default, nullptr},
                  {"setOptions", nullptr, SecureContextSetOptions, nullptr, nullptr, nullptr, napi_default, nullptr},
                  {"setCiphers", nullptr, SecureContextSetCiphers, nullptr, nullptr, nullptr, napi_default, nullptr},
                  {"setCipherSuites", nullptr, SecureContextSetCipherSuites, nullptr, nullptr, nullptr, napi_default,
                   nullptr},
                  {"setCert", nullptr, SecureContextSetCert, nullptr, nullptr, nullptr, napi_default, nullptr},
                  {"setKey", nullptr, SecureContextSetKey, nullptr, nullptr, nullptr, napi_default, nullptr},
                  {"addCACert", nullptr, SecureContextAddCACert, nullptr, nullptr, nullptr, napi_default, nullptr},
                  {"addCRL", nullptr, SecureContextAddCrl, nullptr, nullptr, nullptr, napi_default, nullptr},
                  {"addRootCerts", nullptr, SecureContextNoop, nullptr, nullptr, nullptr, napi_default, nullptr},
                  {"setECDHCurve", nullptr, SecureContextNoop, nullptr, nullptr, nullptr, napi_default, nullptr},
                  {"setDHParam", nullptr, SecureContextNoop, nullptr, nullptr, nullptr, napi_default, nullptr},
                  {"setSigalgs", nullptr, SecureContextNoop, nullptr, nullptr, nullptr, napi_default, nullptr},
                  {"setEngineKey", nullptr, SecureContextNoop, nullptr, nullptr, nullptr, napi_default, nullptr},
                  {"setClientCertEngine", nullptr, SecureContextNoop, nullptr, nullptr, nullptr, napi_default,
                   nullptr},
                  {"setSessionIdContext", nullptr, SecureContextNoop, nullptr, nullptr, nullptr, napi_default,
                   nullptr},
                  {"setSessionTimeout", nullptr, SecureContextNoop, nullptr, nullptr, nullptr, napi_default, nullptr},
                  {"setTicketKeys", nullptr, SecureContextNoop, nullptr, nullptr, nullptr, napi_default, nullptr},
                  {"setAllowPartialTrustChain", nullptr, SecureContextNoop, nullptr, nullptr, nullptr, napi_default,
                   nullptr},
                  {"getCertificate", nullptr, SecureContextNoop, nullptr, nullptr, nullptr, napi_default, nullptr},
                  {"getIssuer", nullptr, SecureContextNoop, nullptr, nullptr, nullptr, napi_default, nullptr},
                  {"setMinProto", nullptr, SecureContextSetMinProto, nullptr, nullptr, nullptr, napi_default, nullptr},
                  {"setMaxProto", nullptr, SecureContextSetMaxProto, nullptr, nullptr, nullptr, napi_default, nullptr},
                  {"loadPKCS12", nullptr, SecureContextLoadPKCS12, nullptr, nullptr, nullptr, napi_default, nullptr},
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
                  {"getAsymmetricKeyType", nullptr, KeyObjectGetAsymmetricKeyType, nullptr, nullptr, nullptr,
                   napi_default, nullptr},
                  {"keyDetail", nullptr, KeyObjectGetAsymmetricKeyDetails, nullptr, nullptr, nullptr, napi_default,
                   nullptr},
                  {"getSymmetricKeySize", nullptr, KeyObjectGetSymmetricKeySize, nullptr, nullptr, nullptr, napi_default,
                   nullptr},
                  {"equals", nullptr, KeyObjectEquals, nullptr, nullptr, nullptr, napi_default, nullptr},
              });

  EnsureClass(env,
              out,
              "DiffieHellman",
              DiffieHellmanCtor,
              {
                  {"generateKeys", nullptr, DiffieHellmanGenerateKeys, nullptr, nullptr, nullptr, napi_default,
                   nullptr},
                  {"computeSecret", nullptr, DiffieHellmanComputeSecret, nullptr, nullptr, nullptr, napi_default,
                   nullptr},
                  {"getPrime", nullptr, DiffieHellmanGetPrime, nullptr, nullptr, nullptr, napi_default, nullptr},
                  {"getGenerator", nullptr, DiffieHellmanGetGenerator, nullptr, nullptr, nullptr, napi_default,
                   nullptr},
                  {"getPublicKey", nullptr, DiffieHellmanGetPublicKey, nullptr, nullptr, nullptr, napi_default,
                   nullptr},
                  {"getPrivateKey", nullptr, DiffieHellmanGetPrivateKey, nullptr, nullptr, nullptr, napi_default,
                   nullptr},
                  {"setPublicKey", nullptr, DiffieHellmanSetPublicKey, nullptr, nullptr, nullptr, napi_default,
                   nullptr},
                  {"setPrivateKey", nullptr, DiffieHellmanSetPrivateKey, nullptr, nullptr, nullptr, napi_default,
                   nullptr},
              });

  EnsureClass(env,
              out,
              "DiffieHellmanGroup",
              DiffieHellmanGroupCtor,
              {
                  {"generateKeys", nullptr, DiffieHellmanGenerateKeys, nullptr, nullptr, nullptr, napi_default,
                   nullptr},
                  {"computeSecret", nullptr, DiffieHellmanComputeSecret, nullptr, nullptr, nullptr, napi_default,
                   nullptr},
                  {"getPrime", nullptr, DiffieHellmanGetPrime, nullptr, nullptr, nullptr, napi_default, nullptr},
                  {"getGenerator", nullptr, DiffieHellmanGetGenerator, nullptr, nullptr, nullptr, napi_default,
                   nullptr},
                  {"getPublicKey", nullptr, DiffieHellmanGetPublicKey, nullptr, nullptr, nullptr, napi_default,
                   nullptr},
                  {"getPrivateKey", nullptr, DiffieHellmanGetPrivateKey, nullptr, nullptr, nullptr, napi_default,
                   nullptr},
              });

  EnsureClass(env,
              out,
              "ECDH",
              EcdhCtor,
              {
                  {"generateKeys", nullptr, EcdhGenerateKeys, nullptr, nullptr, nullptr, napi_default, nullptr},
                  {"computeSecret", nullptr, EcdhComputeSecret, nullptr, nullptr, nullptr, napi_default, nullptr},
                  {"getPublicKey", nullptr, EcdhGetPublicKey, nullptr, nullptr, nullptr, napi_default, nullptr},
                  {"getPrivateKey", nullptr, EcdhGetPrivateKey, nullptr, nullptr, nullptr, napi_default, nullptr},
                  {"setPublicKey", nullptr, EcdhSetPublicKey, nullptr, nullptr, nullptr, napi_default, nullptr},
                  {"setPrivateKey", nullptr, EcdhSetPrivateKey, nullptr, nullptr, nullptr, napi_default, nullptr},
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
              PBKDF2JobCtor,
              {
                  {"run", nullptr, PBKDF2JobRun, nullptr, nullptr, nullptr, napi_default, nullptr},
              });
  EnsureClass(env,
              out,
              "ScryptJob",
              ScryptJobCtor,
              {
                  {"run", nullptr, ScryptJobRun, nullptr, nullptr, nullptr, napi_default, nullptr},
              });
  EnsureClass(env,
              out,
              "HKDFJob",
              HKDFJobCtor,
              {
                  {"run", nullptr, HKDFJobRun, nullptr, nullptr, nullptr, napi_default, nullptr},
              });
  SetClassPrototypeMethod(env, out, "HKDFJob", "run", HKDFJobRun);
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
  EnsureClass(env,
              out,
              "RsaKeyPairGenJob",
              RsaKeyPairGenJobCtor,
              {
                  {"run", nullptr, RsaKeyPairGenJobRun, nullptr, nullptr, nullptr, napi_default, nullptr},
              });
  EnsureClass(env,
              out,
              "DsaKeyPairGenJob",
              JobCtor,
              {
                  {"run", nullptr, DsaKeyPairGenJobRun, nullptr, nullptr, nullptr, napi_default, nullptr},
              });
  EnsureClass(env,
              out,
              "DhKeyPairGenJob",
              DhKeyPairGenJobCtor,
              {
                  {"run", nullptr, DhKeyPairGenJobRun, nullptr, nullptr, nullptr, napi_default, nullptr},
              });
  EnsureClass(env,
              out,
              "EcKeyPairGenJob",
              JobCtor,
              {
                  {"run", nullptr, EcKeyPairGenJobRun, nullptr, nullptr, nullptr, napi_default, nullptr},
              });
  EnsureClass(env,
              out,
              "NidKeyPairGenJob",
              JobCtor,
              {
                  {"run", nullptr, NidKeyPairGenJobRun, nullptr, nullptr, nullptr, napi_default, nullptr},
              });

  const char* stub_classes[] = {
      "AESCipherJob",      "Argon2Job",          "ChaCha20Poly1305CipherJob", "CheckPrimeJob",
      "DHBitsJob",         "DHKeyExportJob",     "ECDHBitsJob",
      "ECDHConvertKey",
      "ECKeyExportJob",    "HmacJob",            "KEMDecapsulateJob",         "KEMEncapsulateJob",
      "KmacJob",           "RSACipherJob",       "RSAKeyExportJob",           "RandomPrimeJob",
      "SecretKeyGenJob",
  };
  for (const char* cls : stub_classes) EnsureStubClass(env, out, cls);

  return out;
}

}  // namespace internal_binding
