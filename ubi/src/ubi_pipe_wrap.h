#ifndef UBI_PIPE_WRAP_H_
#define UBI_PIPE_WRAP_H_

#include <uv.h>

#include "node_api.h"

napi_value UbiInstallPipeWrapBinding(napi_env env);
uv_stream_t* UbiPipeWrapGetStream(napi_env env, napi_value value);

#endif  // UBI_PIPE_WRAP_H_
