#ifndef EDGE_WEBASSEMBLY_EDGE_WASM_H_
#define EDGE_WEBASSEMBLY_EDGE_WASM_H_

#include <string>

#include "node_api.h"

bool EdgeInstallQuickJsWebAssembly(napi_env env, std::string *error_out);
// Returns a structured-clone-safe marker for WebAssembly.Module and shared
// WebAssembly.Memory values, or nullptr when `value` is not cloneable here.
napi_value EdgePrepareQuickJsWebAssemblyClone(napi_env env, napi_value value);
bool EdgeIsQuickJsWebAssemblyCloneMarker(napi_env env, napi_value value);
napi_value EdgeRestoreQuickJsWebAssemblyClone(napi_env env, napi_value marker);

#endif // EDGE_WEBASSEMBLY_EDGE_WASM_H_
