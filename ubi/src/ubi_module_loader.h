#ifndef UBI_MODULE_LOADER_H_
#define UBI_MODULE_LOADER_H_

#include "node_api.h"
#include "ubi_task_queue.h"

napi_status UbiInstallModuleLoader(napi_env env, const char* entry_script_path);

// Store primordials and internalBinding in loader state so they are passed from C++ when calling
// the module wrapper (Node-aligned: fn->Call(context, undefined, argc, argv) with argv from C++).
// Call after the bootstrap prelude so every user module receives the same reference.
void UbiSetPrimordials(napi_env env, napi_value primordials);
void UbiSetInternalBinding(napi_env env, napi_value internal_binding);
void UbiSetPrivateSymbols(napi_env env, napi_value private_symbols);
void UbiSetPerIsolateSymbols(napi_env env, napi_value per_isolate_symbols);
napi_value UbiGetRequireFunction(napi_env env);
napi_value UbiGetInternalBinding(napi_env env);
bool UbiRequireBuiltin(napi_env env, const char* id, napi_value* out);

#endif  // UBI_MODULE_LOADER_H_
