#ifndef UNODE_MODULE_LOADER_H_
#define UNODE_MODULE_LOADER_H_

#include "js_native_api.h"

napi_status UnodeInstallModuleLoader(napi_env env, const char* entry_script_path);

// Override for raw Node tests: when set, used instead of UNODE_FALLBACK_BUILTINS_DIR env.
void UnodeSetFallbackBuiltinsDir(const char* path);

#endif  // UNODE_MODULE_LOADER_H_
