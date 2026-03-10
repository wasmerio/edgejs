#ifndef UBI_SECURE_CONTEXT_BRIDGE_H_
#define UBI_SECURE_CONTEXT_BRIDGE_H_

#include <cstdint>
#include <vector>

#include <openssl/ssl.h>
#include <openssl/x509.h>

#include "node_api.h"

namespace ubi::crypto {

struct SecureContextHolder {
  explicit SecureContextHolder(SSL_CTX* in_ctx)
      : ctx(in_ctx) {}

  ~SecureContextHolder() {
    if (cert != nullptr) {
      X509_free(cert);
      cert = nullptr;
    }
    if (issuer != nullptr) {
      X509_free(issuer);
      issuer = nullptr;
    }
    if (ctx != nullptr) {
      SSL_CTX_free(ctx);
      ctx = nullptr;
    }
  }

  SSL_CTX* ctx = nullptr;
  X509* cert = nullptr;
  X509* issuer = nullptr;
  std::vector<unsigned char> ticket_keys;
  bool ticket_callback_installed = false;
};

inline bool GetSecureContextHolder(napi_env env, napi_value value, SecureContextHolder** out) {
  if (out == nullptr) return false;
  *out = nullptr;
  void* raw = nullptr;
  if (napi_get_value_external(env, value, &raw) != napi_ok || raw == nullptr) return false;
  *out = reinterpret_cast<SecureContextHolder*>(raw);
  return true;
}

}  // namespace ubi::crypto

namespace internal_binding {

bool UbiCryptoGetSecureContextHolderFromObject(napi_env env,
                                               napi_value value,
                                               ubi::crypto::SecureContextHolder** out);

}  // namespace internal_binding

#endif  // UBI_SECURE_CONTEXT_BRIDGE_H_
