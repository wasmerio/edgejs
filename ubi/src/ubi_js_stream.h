#ifndef UBI_JS_STREAM_H_
#define UBI_JS_STREAM_H_

#include "node_api.h"

struct UbiStreamBase;

napi_value UbiInstallJsStreamBinding(napi_env env);
int UbiJsStreamWriteBuffer(UbiStreamBase* base,
                           napi_value req_obj,
                           napi_value payload,
                           bool* async_out);

#endif  // UBI_JS_STREAM_H_
