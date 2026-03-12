#ifndef EDGE_INTERNAL_BINDING_DISPATCH_H_
#define EDGE_INTERNAL_BINDING_DISPATCH_H_

#include <string>

#include "node_api.h"

namespace internal_binding {

struct ResolveCallbacks {
  napi_value (*get_or_create_builtins)(napi_env env, void* state) = nullptr;
  napi_value (*get_or_create_task_queue)(napi_env env) = nullptr;
  napi_value (*get_or_create_errors)(napi_env env) = nullptr;
  napi_value (*get_or_create_trace_events)(napi_env env) = nullptr;

  // Canonical native resolver for concrete binding exports (one source of truth).
  napi_value (*resolve_binding)(napi_env env, void* state, const char* name) = nullptr;

  // Keep complex binding builders in module_loader for now.
  napi_value (*resolve_uv)(napi_env env) = nullptr;
  napi_value (*resolve_contextify)(napi_env env) = nullptr;
  napi_value (*resolve_modules)(napi_env env) = nullptr;
  napi_value (*resolve_options)(napi_env env) = nullptr;
};

struct ResolveOptions {
  void* state = nullptr;
  ResolveCallbacks callbacks;
};

napi_value Resolve(napi_env env, const std::string& name, const ResolveOptions& options);

}  // namespace internal_binding

#endif  // EDGE_INTERNAL_BINDING_DISPATCH_H_
