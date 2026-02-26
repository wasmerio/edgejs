#include "crypto/unode_crypto_binding.h"

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

#include "ncrypto.h"
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/pkcs12.h>
#include <openssl/x509.h>

namespace unode::crypto {
namespace {

std::string ValueToUtf8(napi_env env, napi_value value) {
  size_t len = 0;
  if (napi_get_value_string_utf8(env, value, nullptr, 0, &len) != napi_ok) return "";
  std::string out(len + 1, '\0');
  size_t copied = 0;
  if (napi_get_value_string_utf8(env, value, out.data(), out.size(), &copied) != napi_ok) return "";
  out.resize(copied);
  return out;
}

bool GetBufferBytes(napi_env env, napi_value value, uint8_t** data, size_t* len) {
  bool is_buffer = false;
  if (napi_is_buffer(env, value, &is_buffer) == napi_ok && is_buffer) {
    return napi_get_buffer_info(env, value, reinterpret_cast<void**>(data), len) == napi_ok;
  }
  napi_typedarray_type ta_type = napi_uint8_array;
  size_t element_len = 0;
  void* raw = nullptr;
  napi_value ab = nullptr;
  size_t byte_offset = 0;
  if (napi_get_typedarray_info(env, value, &ta_type, &element_len, &raw, &ab, &byte_offset) == napi_ok &&
      ta_type == napi_uint8_array && raw != nullptr) {
    *data = reinterpret_cast<uint8_t*>(raw);
    *len = element_len;
    return true;
  }
  return false;
}

napi_value MakeError(napi_env env, const char* code, const char* message) {
  napi_value code_v = nullptr;
  napi_value msg_v = nullptr;
  napi_value err_v = nullptr;
  napi_create_string_utf8(env, code, NAPI_AUTO_LENGTH, &code_v);
  napi_create_string_utf8(env, message, NAPI_AUTO_LENGTH, &msg_v);
  napi_create_error(env, code_v, msg_v, &err_v);
  if (err_v != nullptr && code_v != nullptr) napi_set_named_property(env, err_v, "code", code_v);
  return err_v;
}

void ThrowError(napi_env env, const char* code, const char* message) {
  napi_value err = MakeError(env, code, message);
  if (err != nullptr) napi_throw(env, err);
}

void ThrowLastOpenSslError(napi_env env, const char* fallback_code, const char* fallback_message) {
  const unsigned long err = ERR_get_error();
  if (err == 0) {
    ThrowError(env, fallback_code, fallback_message);
    return;
  }
  char buf[256];
  ERR_error_string_n(err, buf, sizeof(buf));
  // Keep openssl-like message surface where possible.
  ThrowError(env, fallback_code, buf);
}

ncrypto::Digest ResolveDigest(const std::string& name) {
  if (name == "RSA-SHA1") return ncrypto::Digest::SHA1;
  return ncrypto::Digest::FromName(name.c_str());
}

ncrypto::Cipher ResolveCipher(const std::string& name) {
  return ncrypto::Cipher::FromName(name.c_str());
}

napi_value CreateBufferCopy(napi_env env, const uint8_t* data, size_t len) {
  napi_value ab = nullptr;
  void* out_data = nullptr;
  if (napi_create_arraybuffer(env, len, &out_data, &ab) != napi_ok || out_data == nullptr || ab == nullptr) {
    return nullptr;
  }
  if (len > 0) std::memcpy(out_data, data, len);
  napi_value out = nullptr;
  if (napi_create_typedarray(env, napi_uint8_array, len, ab, 0, &out) != napi_ok) return nullptr;
  return out;
}

napi_value CreateBufferCopy(napi_env env, const ncrypto::DataPointer& dp) {
  return CreateBufferCopy(env, dp.get<uint8_t>(), dp.size());
}

napi_value CryptoHashOneShot(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value argv[2] = {nullptr, nullptr};
  if (napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr) != napi_ok || argc < 2) return nullptr;
  const std::string algo = ValueToUtf8(env, argv[0]);
  uint8_t* in = nullptr;
  size_t in_len = 0;
  if (!GetBufferBytes(env, argv[1], &in, &in_len)) {
    ThrowError(env, "ERR_INVALID_ARG_TYPE", "hash data must be a Buffer");
    return nullptr;
  }
  const ncrypto::Digest md = ResolveDigest(algo);
  if (!md) {
    ThrowError(env, "ERR_CRYPTO_HASH_UNKNOWN", "Unknown hash");
    return nullptr;
  }
  auto out = ncrypto::hashDigest({in, in_len}, md.get());
  if (!out) {
    ThrowError(env, "ERR_CRYPTO_OPERATION_FAILED", "Hash operation failed");
    return nullptr;
  }
  return CreateBufferCopy(env, out);
}

napi_value CryptoHashOneShotXof(napi_env env, napi_callback_info info) {
  size_t argc = 3;
  napi_value argv[3] = {nullptr, nullptr, nullptr};
  if (napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr) != napi_ok || argc < 3) return nullptr;
  const std::string algo = ValueToUtf8(env, argv[0]);
  uint8_t* in = nullptr;
  size_t in_len = 0;
  if (!GetBufferBytes(env, argv[1], &in, &in_len)) {
    ThrowError(env, "ERR_INVALID_ARG_TYPE", "hash data must be a Buffer");
    return nullptr;
  }
  int32_t out_len_i32 = 0;
  if (napi_get_value_int32(env, argv[2], &out_len_i32) != napi_ok || out_len_i32 < 0) {
    ThrowError(env, "ERR_INVALID_ARG_VALUE", "Invalid output length");
    return nullptr;
  }
  const ncrypto::Digest md = ResolveDigest(algo);
  if (!md) {
    ThrowError(env, "ERR_CRYPTO_HASH_UNKNOWN", "Unknown hash");
    return nullptr;
  }
  auto out = ncrypto::xofHashDigest({in, in_len}, md.get(), static_cast<size_t>(out_len_i32));
  if (!out) {
    ThrowError(env, "ERR_CRYPTO_OPERATION_FAILED", "Hash operation failed");
    return nullptr;
  }
  return CreateBufferCopy(env, out);
}

napi_value CryptoHmacOneShot(napi_env env, napi_callback_info info) {
  size_t argc = 3;
  napi_value argv[3] = {nullptr, nullptr, nullptr};
  if (napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr) != napi_ok || argc < 3) return nullptr;
  const std::string algo = ValueToUtf8(env, argv[0]);
  uint8_t* key = nullptr;
  size_t key_len = 0;
  uint8_t* in = nullptr;
  size_t in_len = 0;
  if (!GetBufferBytes(env, argv[1], &key, &key_len) || !GetBufferBytes(env, argv[2], &in, &in_len)) {
    ThrowError(env, "ERR_INVALID_ARG_TYPE", "hmac key/data must be Buffers");
    return nullptr;
  }
  const ncrypto::Digest md = ResolveDigest(algo);
  if (!md) {
    ThrowError(env, "ERR_CRYPTO_HASH_UNKNOWN", "Unknown digest");
    return nullptr;
  }
  auto hmac = ncrypto::HMACCtxPointer::New();
  if (!hmac || !hmac.init({key, key_len}, md) || !hmac.update({in, in_len})) {
    ThrowError(env, "ERR_CRYPTO_OPERATION_FAILED", "HMAC operation failed");
    return nullptr;
  }
  auto out = hmac.digest();
  if (!out) {
    ThrowError(env, "ERR_CRYPTO_OPERATION_FAILED", "HMAC operation failed");
    return nullptr;
  }
  return CreateBufferCopy(env, out);
}

napi_value CryptoRandomFillSync(napi_env env, napi_callback_info info) {
  size_t argc = 3;
  napi_value argv[3] = {nullptr, nullptr, nullptr};
  if (napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr) != napi_ok || argc < 1) return nullptr;
  uint8_t* data = nullptr;
  size_t len = 0;
  if (!GetBufferBytes(env, argv[0], &data, &len)) {
    ThrowError(env, "ERR_INVALID_ARG_TYPE", "buffer must be a Buffer");
    return nullptr;
  }
  int32_t offset = 0;
  int32_t size = static_cast<int32_t>(len);
  if (argc >= 2 && argv[1] != nullptr) napi_get_value_int32(env, argv[1], &offset);
  if (argc >= 3 && argv[2] != nullptr) napi_get_value_int32(env, argv[2], &size);
  if (offset < 0 || size < 0 || static_cast<size_t>(offset + size) > len) {
    ThrowError(env, "ERR_OUT_OF_RANGE", "offset/size out of range");
    return nullptr;
  }
  if (!ncrypto::CSPRNG(data + offset, static_cast<size_t>(size))) {
    ThrowError(env, "ERR_CRYPTO_OPERATION_FAILED", "CSPRNG failed");
    return nullptr;
  }
  return argv[0];
}

napi_value CryptoRandomBytes(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  if (napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr) != napi_ok || argc < 1) return nullptr;
  int32_t n = 0;
  if (napi_get_value_int32(env, argv[0], &n) != napi_ok || n < 0) {
    ThrowError(env, "ERR_INVALID_ARG_TYPE", "size must be a number >= 0");
    return nullptr;
  }
  napi_value out = nullptr;
  napi_value ab = nullptr;
  void* out_data_raw = nullptr;
  if (napi_create_arraybuffer(env, static_cast<size_t>(n), &out_data_raw, &ab) != napi_ok || ab == nullptr) {
    return nullptr;
  }
  auto* out_data = reinterpret_cast<uint8_t*>(out_data_raw);
  if (n > 0 && !ncrypto::CSPRNG(out_data, static_cast<size_t>(n))) {
    ThrowError(env, "ERR_CRYPTO_OPERATION_FAILED", "CSPRNG failed");
    return nullptr;
  }
  if (napi_create_typedarray(env, napi_uint8_array, static_cast<size_t>(n), ab, 0, &out) != napi_ok) return nullptr;
  return out;
}

napi_value CryptoPbkdf2Sync(napi_env env, napi_callback_info info) {
  size_t argc = 5;
  napi_value argv[5] = {nullptr, nullptr, nullptr, nullptr, nullptr};
  if (napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr) != napi_ok || argc < 5) return nullptr;
  uint8_t* pass = nullptr;
  size_t pass_len = 0;
  uint8_t* salt = nullptr;
  size_t salt_len = 0;
  if (!GetBufferBytes(env, argv[0], &pass, &pass_len) || !GetBufferBytes(env, argv[1], &salt, &salt_len)) {
    ThrowError(env, "ERR_INVALID_ARG_TYPE", "password/salt must be Buffers");
    return nullptr;
  }
  int32_t iter = 0;
  int32_t keylen = 0;
  napi_get_value_int32(env, argv[2], &iter);
  napi_get_value_int32(env, argv[3], &keylen);
  const std::string digest = ValueToUtf8(env, argv[4]);
  const ncrypto::Digest md = ResolveDigest(digest);
  if (!md || iter <= 0 || keylen < 0) {
    ThrowError(env, "ERR_INVALID_ARG_VALUE", "Invalid pbkdf2 arguments");
    return nullptr;
  }
  auto out = ncrypto::pbkdf2(md, {reinterpret_cast<char*>(pass), pass_len}, {salt, salt_len},
                             static_cast<uint32_t>(iter), static_cast<size_t>(keylen));
  if (!out) {
    ThrowError(env, "ERR_CRYPTO_OPERATION_FAILED", "PBKDF2 failed");
    return nullptr;
  }
  return CreateBufferCopy(env, out);
}

napi_value CryptoScryptSync(napi_env env, napi_callback_info info) {
  size_t argc = 7;
  napi_value argv[7] = {nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr};
  if (napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr) != napi_ok || argc < 3) return nullptr;
  uint8_t* pass = nullptr;
  size_t pass_len = 0;
  uint8_t* salt = nullptr;
  size_t salt_len = 0;
  if (!GetBufferBytes(env, argv[0], &pass, &pass_len) || !GetBufferBytes(env, argv[1], &salt, &salt_len)) {
    ThrowError(env, "ERR_INVALID_ARG_TYPE", "password/salt must be Buffers");
    return nullptr;
  }
  int32_t keylen = 0;
  napi_get_value_int32(env, argv[2], &keylen);
  if (keylen < 0) {
    ThrowError(env, "ERR_INVALID_ARG_VALUE", "Invalid key length");
    return nullptr;
  }
  uint64_t N = 16384;
  uint64_t r = 8;
  uint64_t p = 1;
  uint64_t maxmem = 0;
  if (argc >= 4 && argv[3] != nullptr) {
    double v = 0;
    if (napi_get_value_double(env, argv[3], &v) == napi_ok && v > 0) N = static_cast<uint64_t>(v);
  }
  if (argc >= 5 && argv[4] != nullptr) {
    double v = 0;
    if (napi_get_value_double(env, argv[4], &v) == napi_ok && v > 0) r = static_cast<uint64_t>(v);
  }
  if (argc >= 6 && argv[5] != nullptr) {
    double v = 0;
    if (napi_get_value_double(env, argv[5], &v) == napi_ok && v > 0) p = static_cast<uint64_t>(v);
  }
  if (argc >= 7 && argv[6] != nullptr) {
    double v = 0;
    if (napi_get_value_double(env, argv[6], &v) == napi_ok && v > 0) maxmem = static_cast<uint64_t>(v);
  }
  if (!ncrypto::checkScryptParams(N, r, p, maxmem)) {
    ThrowError(env, "ERR_INVALID_ARG_VALUE", "Invalid scrypt params");
    return nullptr;
  }
  auto out = ncrypto::scrypt({reinterpret_cast<char*>(pass), pass_len}, {salt, salt_len},
                             N, r, p, maxmem, static_cast<size_t>(keylen));
  if (!out) {
    // Keep behavior aligned with previous bridge for platforms/OpenSSL builds
    // where ncrypto::scrypt can reject params that EVP_PBE_scrypt accepts.
    std::vector<uint8_t> fallback(static_cast<size_t>(keylen));
    if (EVP_PBE_scrypt(reinterpret_cast<const char*>(pass), pass_len,
                       salt, salt_len, N, r, p, maxmem, fallback.data(), fallback.size()) != 1) {
      ThrowError(env, "ERR_CRYPTO_OPERATION_FAILED", "scrypt failed");
      return nullptr;
    }
    return CreateBufferCopy(env, fallback.data(), fallback.size());
  }
  return CreateBufferCopy(env, out);
}

napi_value CryptoHkdfSync(napi_env env, napi_callback_info info) {
  size_t argc = 5;
  napi_value argv[5] = {nullptr, nullptr, nullptr, nullptr, nullptr};
  if (napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr) != napi_ok || argc < 5) return nullptr;
  const std::string digest = ValueToUtf8(env, argv[0]);
  uint8_t *ikm = nullptr, *salt = nullptr, *info_bytes = nullptr;
  size_t ikm_len = 0, salt_len = 0, info_len = 0;
  if (!GetBufferBytes(env, argv[1], &ikm, &ikm_len) ||
      !GetBufferBytes(env, argv[2], &salt, &salt_len) ||
      !GetBufferBytes(env, argv[3], &info_bytes, &info_len)) {
    ThrowError(env, "ERR_INVALID_ARG_TYPE", "hkdf input/salt/info must be Buffers");
    return nullptr;
  }
  int32_t keylen = 0;
  napi_get_value_int32(env, argv[4], &keylen);
  if (keylen < 0) {
    ThrowError(env, "ERR_INVALID_ARG_VALUE", "Invalid key length");
    return nullptr;
  }
  const ncrypto::Digest md = ResolveDigest(digest);
  if (!md || !ncrypto::checkHkdfLength(md, static_cast<size_t>(keylen))) {
    ThrowError(env, "ERR_INVALID_ARG_VALUE", "Invalid hkdf arguments");
    return nullptr;
  }
  auto out = ncrypto::hkdf(md, {ikm, ikm_len}, {info_bytes, info_len}, {salt, salt_len},
                           static_cast<size_t>(keylen));
  if (!out) {
    ThrowError(env, "ERR_CRYPTO_OPERATION_FAILED", "hkdf failed");
    return nullptr;
  }
  return CreateBufferCopy(env, out);
}

napi_value CryptoCipherTransform(napi_env env, napi_callback_info info) {
  size_t argc = 6;
  napi_value argv[6] = {nullptr, nullptr, nullptr, nullptr, nullptr, nullptr};
  if (napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr) != napi_ok || argc < 6) return nullptr;
  const std::string algo = ValueToUtf8(env, argv[0]);
  uint8_t *key = nullptr, *iv = nullptr, *input = nullptr;
  size_t key_len = 0, iv_len = 0, in_len = 0;
  if (!GetBufferBytes(env, argv[1], &key, &key_len)) return nullptr;

  bool iv_is_null = false;
  napi_valuetype iv_type = napi_undefined;
  if (napi_typeof(env, argv[2], &iv_type) == napi_ok && iv_type == napi_null) {
    iv_is_null = true;
  } else if (!GetBufferBytes(env, argv[2], &iv, &iv_len)) {
    return nullptr;
  }
  if (!GetBufferBytes(env, argv[3], &input, &in_len)) return nullptr;
  bool decrypt = false;
  napi_get_value_bool(env, argv[4], &decrypt);

  const ncrypto::Cipher cipher = ResolveCipher(algo);
  if (!cipher) {
    ThrowError(env, "ERR_CRYPTO_UNKNOWN_CIPHER", "Unknown cipher");
    return nullptr;
  }
  if (cipher.getKeyLength() > 0 && key_len != static_cast<size_t>(cipher.getKeyLength())) {
    ThrowError(env, "ERR_CRYPTO_INVALID_KEYLEN", "Invalid key length");
    return nullptr;
  }
  if (!iv_is_null && cipher.getIvLength() >= 0 && iv_len != static_cast<size_t>(cipher.getIvLength())) {
    ThrowError(env, "ERR_CRYPTO_INVALID_IV", "Invalid IV length");
    return nullptr;
  }

  auto ctx = ncrypto::CipherCtxPointer::New();
  if (!ctx || !ctx.init(cipher, !decrypt, key, iv_is_null ? nullptr : iv)) {
    ThrowError(env, "ERR_CRYPTO_OPERATION_FAILED", "cipher initialization failed");
    return nullptr;
  }

  std::vector<uint8_t> out(in_len + std::max(32, ctx.getBlockSize() + 16));
  int out1 = 0;
  int out2 = 0;
  if (!ctx.update({input, in_len}, out.data(), &out1, false) ||
      !ctx.update({nullptr, 0}, out.data() + out1, &out2, true)) {
    ThrowError(env, "ERR_CRYPTO_OPERATION_FAILED", "cipher operation failed");
    return nullptr;
  }
  return CreateBufferCopy(env, out.data(), static_cast<size_t>(out1 + out2));
}

napi_value CryptoGetHashes(napi_env env, napi_callback_info info) {
  static const char* kHashes[] = {"RSA-SHA1", "sha1", "sha224", "sha256", "sha384", "sha512", "shake128",
                                  "shake256"};
  napi_value arr = nullptr;
  napi_create_array_with_length(env, sizeof(kHashes) / sizeof(kHashes[0]), &arr);
  for (uint32_t i = 0; i < sizeof(kHashes) / sizeof(kHashes[0]); i++) {
    napi_value s = nullptr;
    napi_create_string_utf8(env, kHashes[i], NAPI_AUTO_LENGTH, &s);
    napi_set_element(env, arr, i, s);
  }
  return arr;
}

napi_value CryptoGetCiphers(napi_env env, napi_callback_info info) {
  std::vector<std::string> names;
  ncrypto::Cipher::ForEach([&names](const char* name) { names.emplace_back(name); });
  std::sort(names.begin(), names.end());
  names.erase(std::unique(names.begin(), names.end()), names.end());
  napi_value arr = nullptr;
  napi_create_array_with_length(env, names.size(), &arr);
  for (uint32_t i = 0; i < names.size(); i++) {
    napi_value s = nullptr;
    napi_create_string_utf8(env, names[i].c_str(), NAPI_AUTO_LENGTH, &s);
    napi_set_element(env, arr, i, s);
  }
  return arr;
}

napi_value CryptoGetCurves(napi_env env, napi_callback_info info) {
  std::vector<std::string> curves;
  ncrypto::Ec::GetCurves([&curves](const char* name) {
    if (name != nullptr) curves.emplace_back(name);
    return true;
  });
  std::sort(curves.begin(), curves.end());
  curves.erase(std::unique(curves.begin(), curves.end()), curves.end());
  napi_value arr = nullptr;
  napi_create_array_with_length(env, curves.size(), &arr);
  for (uint32_t i = 0; i < curves.size(); i++) {
    napi_value s = nullptr;
    napi_create_string_utf8(env, curves[i].c_str(), NAPI_AUTO_LENGTH, &s);
    napi_set_element(env, arr, i, s);
  }
  return arr;
}

napi_value CryptoGetCipherInfo(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  if (napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr) != napi_ok || argc < 1) return nullptr;
  const std::string algo = ValueToUtf8(env, argv[0]);
  const ncrypto::Cipher c = ResolveCipher(algo);
  if (!c) {
    napi_value undefined = nullptr;
    napi_get_undefined(env, &undefined);
    return undefined;
  }
  napi_value obj = nullptr;
  napi_create_object(env, &obj);
  napi_value key_len = nullptr;
  napi_value iv_len = nullptr;
  napi_create_int32(env, c.getKeyLength(), &key_len);
  napi_create_int32(env, c.getIvLength(), &iv_len);
  napi_set_named_property(env, obj, "keyLength", key_len);
  napi_set_named_property(env, obj, "ivLength", iv_len);
  const std::string mode(c.getModeLabel().empty() ? "unknown" : c.getModeLabel());
  napi_value mode_v = nullptr;
  napi_create_string_utf8(env, mode.c_str(), NAPI_AUTO_LENGTH, &mode_v);
  napi_set_named_property(env, obj, "mode", mode_v);
  return obj;
}

napi_value CryptoParsePfx(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value argv[2] = {nullptr, nullptr};
  if (napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr) != napi_ok || argc < 1) return nullptr;
  uint8_t* pfx = nullptr;
  size_t pfx_len = 0;
  if (!GetBufferBytes(env, argv[0], &pfx, &pfx_len)) return nullptr;
  std::string pass;
  if (argc >= 2 && argv[1] != nullptr) pass = ValueToUtf8(env, argv[1]);
  const unsigned char* p = pfx;
  PKCS12* pkcs12 = d2i_PKCS12(nullptr, &p, static_cast<long>(pfx_len));
  if (pkcs12 == nullptr) {
    ThrowError(env, "ERR_CRYPTO_PFX", "not enough data");
    return nullptr;
  }
  EVP_PKEY* pkey = nullptr;
  X509* cert = nullptr;
  STACK_OF(X509)* ca = nullptr;
  const int ok = PKCS12_parse(pkcs12, pass.empty() ? nullptr : pass.c_str(), &pkey, &cert, &ca);
  PKCS12_free(pkcs12);
  if (pkey) EVP_PKEY_free(pkey);
  if (cert) X509_free(cert);
  if (ca) sk_X509_pop_free(ca, X509_free);
  if (ok != 1) {
    ThrowError(env, "ERR_CRYPTO_PFX", "mac verify failure");
    return nullptr;
  }
  napi_value true_v = nullptr;
  napi_get_boolean(env, true, &true_v);
  return true_v;
}

napi_value CryptoParseCrl(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  if (napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr) != napi_ok || argc < 1) return nullptr;
  uint8_t* crl_data = nullptr;
  size_t crl_len = 0;
  if (!GetBufferBytes(env, argv[0], &crl_data, &crl_len)) return nullptr;
  BIO* bio = BIO_new_mem_buf(crl_data, static_cast<int>(crl_len));
  X509_CRL* crl = bio ? PEM_read_bio_X509_CRL(bio, nullptr, nullptr, nullptr) : nullptr;
  if (crl == nullptr && bio != nullptr) {
    (void)BIO_reset(bio);
    crl = d2i_X509_CRL_bio(bio, nullptr);
  }
  if (crl) X509_CRL_free(crl);
  if (bio) BIO_free(bio);
  if (crl == nullptr) {
    ThrowError(env, "ERR_CRYPTO_CRL", "Failed to parse CRL");
    return nullptr;
  }
  napi_value true_v = nullptr;
  napi_get_boolean(env, true, &true_v);
  return true_v;
}

EVP_PKEY* ParsePrivateKey(const uint8_t* data, size_t len) {
  BIO* bio = BIO_new_mem_buf(data, static_cast<int>(len));
  if (bio == nullptr) return nullptr;
  EVP_PKEY* pkey = PEM_read_bio_PrivateKey(bio, nullptr, nullptr, nullptr);
  BIO_free(bio);
  return pkey;
}

EVP_PKEY* ParsePublicKeyOrCert(const uint8_t* data, size_t len) {
  BIO* bio = BIO_new_mem_buf(data, static_cast<int>(len));
  if (bio == nullptr) return nullptr;
  EVP_PKEY* pkey = PEM_read_bio_PUBKEY(bio, nullptr, nullptr, nullptr);
  if (pkey == nullptr) {
    (void)BIO_reset(bio);
    X509* cert = PEM_read_bio_X509(bio, nullptr, nullptr, nullptr);
    if (cert != nullptr) {
      pkey = X509_get_pubkey(cert);
      X509_free(cert);
    }
  }
  BIO_free(bio);
  return pkey;
}

napi_value CryptoGetAsymmetricKeyDetails(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  if (napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr) != napi_ok || argc < 1) return nullptr;

  uint8_t* key_bytes = nullptr;
  size_t key_len = 0;
  if (!GetBufferBytes(env, argv[0], &key_bytes, &key_len)) {
    ThrowError(env, "ERR_INVALID_ARG_TYPE", "key must be a Buffer");
    return nullptr;
  }

  EVP_PKEY* pkey = ParsePrivateKey(key_bytes, key_len);
  if (pkey == nullptr) pkey = ParsePublicKeyOrCert(key_bytes, key_len);
  if (pkey == nullptr) {
    napi_value null_v = nullptr;
    napi_get_null(env, &null_v);
    return null_v;
  }

  napi_value out = nullptr;
  if (napi_create_object(env, &out) != napi_ok || out == nullptr) {
    EVP_PKEY_free(pkey);
    return nullptr;
  }

  const int pkey_type = EVP_PKEY_base_id(pkey);
  if (pkey_type == EVP_PKEY_RSA || pkey_type == EVP_PKEY_RSA_PSS) {
    const int bits = EVP_PKEY_bits(pkey);
    if (bits > 0) {
      napi_value bits_v = nullptr;
      if (napi_create_int32(env, bits, &bits_v) == napi_ok && bits_v != nullptr) {
        napi_set_named_property(env, out, "modulusLength", bits_v);
      }
    }
  }

  EVP_PKEY_free(pkey);
  return out;
}

napi_value CryptoSignOneShot(napi_env env, napi_callback_info info) {
  size_t argc = 5;
  napi_value argv[5] = {nullptr, nullptr, nullptr, nullptr, nullptr};
  if (napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr) != napi_ok || argc < 3) return nullptr;

  bool null_digest = false;
  napi_valuetype digest_type = napi_undefined;
  if (napi_typeof(env, argv[0], &digest_type) == napi_ok) {
    null_digest = (digest_type == napi_null || digest_type == napi_undefined);
  }
  const std::string algo = null_digest ? std::string() : ValueToUtf8(env, argv[0]);
  uint8_t* data = nullptr;
  size_t data_len = 0;
  uint8_t* key_pem = nullptr;
  size_t key_len = 0;
  if (!GetBufferBytes(env, argv[1], &data, &data_len) || !GetBufferBytes(env, argv[2], &key_pem, &key_len)) {
    ThrowError(env, "ERR_INVALID_ARG_TYPE", "data and key must be Buffers");
    return nullptr;
  }

  int32_t padding = 0;
  int32_t salt_len = INT32_MIN;
  napi_valuetype padding_type = napi_undefined;
  napi_valuetype salt_type = napi_undefined;
  if (argc >= 4 && argv[3] != nullptr &&
      napi_typeof(env, argv[3], &padding_type) == napi_ok &&
      padding_type == napi_number) {
    napi_get_value_int32(env, argv[3], &padding);
  }
  if (argc >= 5 && argv[4] != nullptr &&
      napi_typeof(env, argv[4], &salt_type) == napi_ok &&
      salt_type == napi_number) {
    napi_get_value_int32(env, argv[4], &salt_len);
  }

  EVP_PKEY* pkey = ParsePrivateKey(key_pem, key_len);
  if (pkey == nullptr) {
    ThrowLastOpenSslError(env, "ERR_CRYPTO_SIGN_KEY_REQUIRED", "No key provided to sign");
    return nullptr;
  }
  const ncrypto::Digest md = null_digest ? ncrypto::Digest(nullptr) : ResolveDigest(algo);
  if (!null_digest && !md) {
    EVP_PKEY_free(pkey);
    ThrowError(env, "ERR_CRYPTO_INVALID_DIGEST", "Invalid digest");
    return nullptr;
  }

  EVP_MD_CTX* mctx = EVP_MD_CTX_new();
  if (mctx == nullptr) {
    EVP_PKEY_free(pkey);
    ThrowError(env, "ERR_CRYPTO_OPERATION_FAILED", "Failed to create digest context");
    return nullptr;
  }
  EVP_PKEY_CTX* pctx = nullptr;
  bool ok = EVP_DigestSignInit(mctx, &pctx, null_digest ? nullptr : md.get(), nullptr, pkey) == 1;
  const int pkey_type = EVP_PKEY_base_id(pkey);
  const bool is_rsa_family = (pkey_type == EVP_PKEY_RSA || pkey_type == EVP_PKEY_RSA_PSS);
  const bool is_ed_key = (pkey_type == EVP_PKEY_ED25519 || pkey_type == EVP_PKEY_ED448);
  int effective_padding = padding;
  if (is_rsa_family && effective_padding == 0 && pkey_type == EVP_PKEY_RSA_PSS) {
    // RSA-PSS keys require PSS padding even when callers omit padding.
    effective_padding = RSA_PKCS1_PSS_PADDING;
  }
  if (ok && pctx != nullptr && is_rsa_family && effective_padding != 0) {
    ok = EVP_PKEY_CTX_set_rsa_padding(pctx, effective_padding) == 1;
    if (ok && effective_padding == RSA_PKCS1_PSS_PADDING) {
      const int effective_salt_len = (salt_len == INT32_MIN) ? RSA_PSS_SALTLEN_MAX_SIGN : salt_len;
      ok = EVP_PKEY_CTX_set_rsa_pss_saltlen(pctx, effective_salt_len) == 1;
    }
  }
  if (ok && is_ed_key && !null_digest) {
    ok = false;
    ThrowError(env, "ERR_CRYPTO_UNSUPPORTED_OPERATION", "Unsupported crypto operation");
  }
  size_t sig_len = 0;
  std::vector<uint8_t> sig;
  if (ok && is_ed_key) {
    ok = EVP_DigestSign(mctx, nullptr, &sig_len, data, data_len) == 1;
    if (ok) {
      sig.resize(sig_len);
      ok = EVP_DigestSign(mctx, sig.data(), &sig_len, data, data_len) == 1;
    }
  } else {
    if (ok) ok = EVP_DigestSignUpdate(mctx, data, data_len) == 1;
    if (ok) ok = EVP_DigestSignFinal(mctx, nullptr, &sig_len) == 1;
    if (ok) {
      sig.resize(sig_len);
      ok = EVP_DigestSignFinal(mctx, sig.data(), &sig_len) == 1;
    }
  }
  EVP_MD_CTX_free(mctx);
  EVP_PKEY_free(pkey);

  if (!ok) {
    ThrowLastOpenSslError(env, "ERR_CRYPTO_OPERATION_FAILED", "sign failed");
    return nullptr;
  }
  return CreateBufferCopy(env, sig.data(), sig_len);
}

napi_value CryptoVerifyOneShot(napi_env env, napi_callback_info info) {
  size_t argc = 6;
  napi_value argv[6] = {nullptr, nullptr, nullptr, nullptr, nullptr, nullptr};
  if (napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr) != napi_ok || argc < 4) return nullptr;

  bool null_digest = false;
  napi_valuetype digest_type = napi_undefined;
  if (napi_typeof(env, argv[0], &digest_type) == napi_ok) {
    null_digest = (digest_type == napi_null || digest_type == napi_undefined);
  }
  const std::string algo = null_digest ? std::string() : ValueToUtf8(env, argv[0]);
  uint8_t* data = nullptr;
  size_t data_len = 0;
  uint8_t* key_pem = nullptr;
  size_t key_len = 0;
  uint8_t* sig = nullptr;
  size_t sig_len = 0;
  if (!GetBufferBytes(env, argv[1], &data, &data_len) ||
      !GetBufferBytes(env, argv[2], &key_pem, &key_len) ||
      !GetBufferBytes(env, argv[3], &sig, &sig_len)) {
    ThrowError(env, "ERR_INVALID_ARG_TYPE", "data, key and signature must be Buffers");
    return nullptr;
  }
  int32_t padding = 0;
  int32_t salt_len = INT32_MIN;
  napi_valuetype padding_type = napi_undefined;
  napi_valuetype salt_type = napi_undefined;
  if (argc >= 5 && argv[4] != nullptr &&
      napi_typeof(env, argv[4], &padding_type) == napi_ok &&
      padding_type == napi_number) {
    napi_get_value_int32(env, argv[4], &padding);
  }
  if (argc >= 6 && argv[5] != nullptr &&
      napi_typeof(env, argv[5], &salt_type) == napi_ok &&
      salt_type == napi_number) {
    napi_get_value_int32(env, argv[5], &salt_len);
  }

  EVP_PKEY* pkey = ParsePublicKeyOrCert(key_pem, key_len);
  if (pkey == nullptr) pkey = ParsePrivateKey(key_pem, key_len);
  if (pkey == nullptr) {
    ThrowLastOpenSslError(env, "ERR_INVALID_ARG_TYPE", "Invalid verify key");
    return nullptr;
  }
  const ncrypto::Digest md = null_digest ? ncrypto::Digest(nullptr) : ResolveDigest(algo);
  if (!null_digest && !md) {
    EVP_PKEY_free(pkey);
    ThrowError(env, "ERR_CRYPTO_INVALID_DIGEST", "Invalid digest");
    return nullptr;
  }

  EVP_MD_CTX* mctx = EVP_MD_CTX_new();
  if (mctx == nullptr) {
    EVP_PKEY_free(pkey);
    ThrowError(env, "ERR_CRYPTO_OPERATION_FAILED", "Failed to create digest context");
    return nullptr;
  }
  EVP_PKEY_CTX* pctx = nullptr;
  bool ok = EVP_DigestVerifyInit(mctx, &pctx, null_digest ? nullptr : md.get(), nullptr, pkey) == 1;
  const int pkey_type = EVP_PKEY_base_id(pkey);
  const bool is_rsa_family = (pkey_type == EVP_PKEY_RSA || pkey_type == EVP_PKEY_RSA_PSS);
  const bool is_ed_key = (pkey_type == EVP_PKEY_ED25519 || pkey_type == EVP_PKEY_ED448);
  int effective_padding = padding;
  if (is_rsa_family && effective_padding == 0 && pkey_type == EVP_PKEY_RSA_PSS) {
    effective_padding = RSA_PKCS1_PSS_PADDING;
  }
  if (ok && pctx != nullptr && is_rsa_family && effective_padding != 0) {
    ok = EVP_PKEY_CTX_set_rsa_padding(pctx, effective_padding) == 1;
    if (ok && effective_padding == RSA_PKCS1_PSS_PADDING && salt_len != INT32_MIN) {
      ok = EVP_PKEY_CTX_set_rsa_pss_saltlen(pctx, salt_len) == 1;
    }
  }
  int vr = 0;
  if (ok && is_ed_key && !null_digest) {
    ok = false;
    ThrowError(env, "ERR_CRYPTO_UNSUPPORTED_OPERATION", "Unsupported crypto operation");
  }
  if (ok && is_ed_key) {
    vr = EVP_DigestVerify(mctx, sig, sig_len, data, data_len);
  } else {
    if (ok) ok = EVP_DigestVerifyUpdate(mctx, data, data_len) == 1;
    vr = ok ? EVP_DigestVerifyFinal(mctx, sig, sig_len) : 0;
  }
  EVP_MD_CTX_free(mctx);
  EVP_PKEY_free(pkey);

  napi_value out = nullptr;
  napi_get_boolean(env, vr == 1, &out);
  return out;
}

void SetMethod(napi_env env, napi_value obj, const char* name, napi_callback fn) {
  napi_value method = nullptr;
  if (napi_create_function(env, name, NAPI_AUTO_LENGTH, fn, nullptr, &method) == napi_ok && method != nullptr) {
    napi_set_named_property(env, obj, name, method);
  }
}

}  // namespace

void InstallCryptoBinding(napi_env env) {
  napi_value global = nullptr;
  if (napi_get_global(env, &global) != napi_ok || global == nullptr) return;
  napi_value binding = nullptr;
  if (napi_create_object(env, &binding) != napi_ok || binding == nullptr) return;

  SetMethod(env, binding, "hashOneShot", CryptoHashOneShot);
  SetMethod(env, binding, "hashOneShotXof", CryptoHashOneShotXof);
  SetMethod(env, binding, "hmacOneShot", CryptoHmacOneShot);
  SetMethod(env, binding, "randomFillSync", CryptoRandomFillSync);
  SetMethod(env, binding, "randomBytes", CryptoRandomBytes);
  SetMethod(env, binding, "pbkdf2Sync", CryptoPbkdf2Sync);
  SetMethod(env, binding, "scryptSync", CryptoScryptSync);
  SetMethod(env, binding, "hkdfSync", CryptoHkdfSync);
  SetMethod(env, binding, "cipherTransform", CryptoCipherTransform);
  SetMethod(env, binding, "getHashes", CryptoGetHashes);
  SetMethod(env, binding, "getCiphers", CryptoGetCiphers);
  SetMethod(env, binding, "getCurves", CryptoGetCurves);
  SetMethod(env, binding, "getCipherInfo", CryptoGetCipherInfo);
  SetMethod(env, binding, "parsePfx", CryptoParsePfx);
  SetMethod(env, binding, "parseCrl", CryptoParseCrl);
  SetMethod(env, binding, "signOneShot", CryptoSignOneShot);
  SetMethod(env, binding, "verifyOneShot", CryptoVerifyOneShot);
  SetMethod(env, binding, "getAsymmetricKeyDetails", CryptoGetAsymmetricKeyDetails);

  napi_set_named_property(env, global, "__unode_crypto_binding", binding);
}

}  // namespace unode::crypto
