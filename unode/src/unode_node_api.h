#ifndef UNODE_NODE_API_H_
#define UNODE_NODE_API_H_

#include "js_native_api.h"

napi_status UnodeInstallConsole(napi_env env);
napi_status UnodeInstallUnofficialNapiTestingUntilGc(napi_env env, napi_value target);

#endif  // UNODE_NODE_API_H_
