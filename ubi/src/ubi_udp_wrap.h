#ifndef UBI_UDP_WRAP_H_
#define UBI_UDP_WRAP_H_

#include <uv.h>

#include "node_api.h"
#include "ubi_udp_listener.h"

napi_value UbiInstallUdpWrapBinding(napi_env env);
napi_value UbiGetUdpWrapConstructor(napi_env env);
uv_handle_t* UbiUdpWrapGetHandle(napi_env env, napi_value value);
UbiUdpSendWrap* UbiUdpWrapUnwrapSendWrap(napi_env env, napi_value value);

#endif  // UBI_UDP_WRAP_H_
