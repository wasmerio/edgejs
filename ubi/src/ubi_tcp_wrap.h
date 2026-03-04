#ifndef UBI_TCP_WRAP_H_
#define UBI_TCP_WRAP_H_

#include "node_api.h"
#include <uv.h>

napi_value UbiInstallTcpWrapBinding(napi_env env);
napi_value UbiGetTcpWrapConstructor(napi_env env);
uv_stream_t* UbiTcpWrapGetStream(napi_env env, napi_value value);

#endif  // UBI_TCP_WRAP_H_
