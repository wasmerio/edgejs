#include "ubi_crypto.h"

#include "crypto/ubi_crypto_binding.h"

napi_value UbiInstallCryptoBinding(napi_env env) {
  return ubi::crypto::InstallCryptoBinding(env);
}
