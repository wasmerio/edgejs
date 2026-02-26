#include "unode_crypto.h"

#include "crypto/unode_crypto_binding.h"

void UnodeInstallCryptoBinding(napi_env env) {
  unode::crypto::InstallCryptoBinding(env);
  napi_value global = nullptr;
  napi_value binding = nullptr;
  if (napi_get_global(env, &global) != napi_ok || global == nullptr) return;
  if (napi_get_named_property(env, global, "__unode_crypto_binding", &binding) != napi_ok || binding == nullptr) {
    return;
  }
  napi_set_named_property(env, global, "__unode_crypto", binding);
}
