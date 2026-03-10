#ifndef UBI_CRYPTO_BINDING_H_
#define UBI_CRYPTO_BINDING_H_

#include "node_api.h"

namespace ubi::crypto {

napi_value InstallCryptoBinding(napi_env env);
napi_value CryptoGetBundledRootCertificates(napi_env env, napi_callback_info info);
napi_value CryptoGetExtraCACertificates(napi_env env, napi_callback_info info);
napi_value CryptoGetSystemCACertificates(napi_env env, napi_callback_info info);
napi_value CryptoGetUserRootCertificates(napi_env env, napi_callback_info info);
napi_value CryptoResetRootCertStore(napi_env env, napi_callback_info info);
napi_value CryptoStartLoadingCertificatesOffThread(napi_env env, napi_callback_info info);

}  // namespace ubi::crypto

#endif  // UBI_CRYPTO_BINDING_H_
