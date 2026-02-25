#include "napi_v8_unofficial_testing.h"

#include <memory>
#include <mutex>
#include <new>
#include <optional>

#include <libplatform/libplatform.h>
#include <v8.h>

#include "napi_v8_platform.h"

namespace {

struct SharedRuntime {
  std::unique_ptr<v8::Platform> platform;
  v8::Isolate::CreateParams params{};
  v8::Isolate* isolate = nullptr;
  uint32_t refcount = 0;
};

struct UnofficialEnvScope {
  v8::Isolate* isolate = nullptr;
  v8::Isolate::Scope isolate_scope;
  v8::HandleScope handle_scope;
  std::optional<v8::Global<v8::Context>> context;
  std::optional<v8::Context::Scope> context_scope;
  napi_env env = nullptr;

  explicit UnofficialEnvScope(v8::Isolate* isolate_in)
      : isolate(isolate_in),
        isolate_scope(isolate_in),
        handle_scope(isolate_in) {}

  ~UnofficialEnvScope() {
    if (context.has_value()) {
      context->Reset();
    }
  }
};

std::mutex g_runtime_mu;
SharedRuntime g_runtime;

napi_status AcquireRuntime(v8::Isolate** isolate_out) {
  if (isolate_out == nullptr) return napi_invalid_arg;
  std::lock_guard<std::mutex> lock(g_runtime_mu);

  if (g_runtime.refcount == 0) {
    v8::V8::InitializeICUDefaultLocation("");
    v8::V8::InitializeExternalStartupData("");
    g_runtime.platform = v8::platform::NewDefaultPlatform();
    v8::V8::InitializePlatform(g_runtime.platform.get());
    v8::V8::Initialize();

    g_runtime.params.array_buffer_allocator =
        v8::ArrayBuffer::Allocator::NewDefaultAllocator();
    g_runtime.isolate = v8::Isolate::New(g_runtime.params);
    if (g_runtime.isolate == nullptr) {
      delete g_runtime.params.array_buffer_allocator;
      g_runtime.params.array_buffer_allocator = nullptr;
      g_runtime.platform.reset();
      v8::V8::Dispose();
      v8::V8::DisposePlatform();
      return napi_generic_failure;
    }
  }

  g_runtime.refcount++;
  *isolate_out = g_runtime.isolate;
  return napi_ok;
}

void ReleaseRuntime() {
  std::lock_guard<std::mutex> lock(g_runtime_mu);
  if (g_runtime.refcount == 0) return;

  g_runtime.refcount--;
  // Intentionally keep shared V8 runtime alive for process lifetime in tests.
  // Repeated Dispose()/Initialize cycles can trigger instability with embedded V8.
}

}  // namespace

extern "C" napi_status NAPI_CDECL unofficial_napi_v8_open_env_scope(
    int32_t module_api_version, napi_env* env_out, void** scope_out) {
  if (env_out == nullptr || scope_out == nullptr) return napi_invalid_arg;

  v8::Isolate* isolate = nullptr;
  napi_status status = AcquireRuntime(&isolate);
  if (status != napi_ok) return status;

  auto* scope = new (std::nothrow) UnofficialEnvScope(isolate);
  if (scope == nullptr) {
    ReleaseRuntime();
    return napi_generic_failure;
  }

  v8::Local<v8::Context> context = v8::Context::New(isolate);
  scope->context.emplace(isolate, context);
  scope->context_scope.emplace(context);
  status = napi_v8_create_env(context, module_api_version, &scope->env);
  if (status != napi_ok || scope->env == nullptr) {
    delete scope;
    ReleaseRuntime();
    return (status == napi_ok) ? napi_generic_failure : status;
  }

  *env_out = scope->env;
  *scope_out = scope;
  return napi_ok;
}

extern "C" napi_status NAPI_CDECL unofficial_napi_v8_close_env_scope(void* scope_ptr) {
  if (scope_ptr == nullptr) return napi_invalid_arg;
  auto* scope = static_cast<UnofficialEnvScope*>(scope_ptr);

  napi_status status = napi_ok;
  if (scope->env != nullptr) {
    status = napi_v8_destroy_env(scope->env);
    scope->env = nullptr;
  }
  delete scope;
  ReleaseRuntime();
  return status;
}
