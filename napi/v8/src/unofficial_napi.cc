#include "unofficial_napi.h"

#include <ctime>
#include <cstdio>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <unordered_map>

#include <libplatform/libplatform.h>

#include "internal/napi_v8_env.h"

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
      : isolate(isolate_in), isolate_scope(isolate_in), handle_scope(isolate_in) {}

  ~UnofficialEnvScope() {
    if (context.has_value()) {
      context->Reset();
    }
  }
};

std::mutex g_runtime_mu;
SharedRuntime g_runtime;
std::unordered_map<v8::Isolate*, napi_env> g_env_by_isolate;
std::unordered_map<v8::Isolate*, v8::Global<v8::Function>> g_promise_reject_callbacks;

void ApplyDefaultV8Flags() {
  static constexpr char kDefaultFlags[] = "--js-float16array";
  v8::V8::SetFlagsFromString(kDefaultFlags, static_cast<int>(sizeof(kDefaultFlags) - 1));
}

napi_status AcquireRuntime(v8::Isolate** isolate_out) {
  if (isolate_out == nullptr) return napi_invalid_arg;
  std::lock_guard<std::mutex> lock(g_runtime_mu);

  // Only initialize V8 and create the isolate once per process (when refcount
  // was 0 and we don't have an isolate yet). Re-initializing causes V8 fatal
  // "Wrong initialization order" when a second test runs after the first released.
  if (g_runtime.refcount == 0 && g_runtime.isolate == nullptr) {
    ApplyDefaultV8Flags();
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
  // Keep shared V8 runtime alive for process lifetime in tests to avoid
  // repeated Dispose/Initialize instability in embedded setups.
}

void PromiseRejectCallback(v8::PromiseRejectMessage message) {
  v8::Local<v8::Promise> promise = message.GetPromise();
  if (promise.IsEmpty()) return;

  v8::Isolate* isolate = promise->GetIsolate();
  napi_env env = nullptr;
  v8::Local<v8::Function> callback;
  {
    std::lock_guard<std::mutex> lock(g_runtime_mu);
    const auto env_it = g_env_by_isolate.find(isolate);
    if (env_it == g_env_by_isolate.end() || env_it->second == nullptr) return;
    env = env_it->second;
    const auto cb_it = g_promise_reject_callbacks.find(isolate);
    if (cb_it == g_promise_reject_callbacks.end() || cb_it->second.IsEmpty()) return;
    callback = cb_it->second.Get(isolate);
  }
  if (env == nullptr || callback.IsEmpty()) return;

  v8::PromiseRejectEvent event = message.GetEvent();
  v8::Local<v8::Value> value;
  switch (event) {
    case v8::kPromiseRejectWithNoHandler:
    case v8::kPromiseResolveAfterResolved:
    case v8::kPromiseRejectAfterResolved:
      value = message.GetValue();
      if (value.IsEmpty()) value = v8::Undefined(isolate);
      break;
    case v8::kPromiseHandlerAddedAfterReject:
      value = v8::Undefined(isolate);
      break;
    default:
      return;
  }

  v8::HandleScope handle_scope(isolate);
  v8::Local<v8::Context> context = env->context();
  v8::Context::Scope context_scope(context);
  v8::TryCatch tc(isolate);
  v8::Local<v8::Value> args[] = {
      v8::Integer::New(isolate, static_cast<int>(event)),
      promise,
      value};
  (void)callback->Call(context, v8::Undefined(isolate), 3, args);
  if (tc.HasCaught() && !tc.HasTerminated()) {
    // Match Node behavior: V8 expects this callback to return without a pending
    // exception. Print a best-effort diagnostic instead of scheduling it.
    std::fprintf(stderr, "Exception in PromiseRejectCallback:\n");
    v8::Local<v8::Value> caught = tc.Exception();
    v8::String::Utf8Value text(isolate, caught);
    if (*text != nullptr) {
      std::fprintf(stderr, "%s\n", *text);
    } else {
      std::fprintf(stderr, "<exception>\n");
    }
  }
}

}  // namespace

extern "C" {

napi_status NAPI_CDECL unofficial_napi_create_env_from_context(
    v8::Local<v8::Context> context, int32_t module_api_version, napi_env* result) {
  if (result == nullptr || context.IsEmpty()) return napi_invalid_arg;
  context->GetIsolate()->SetMicrotasksPolicy(v8::MicrotasksPolicy::kExplicit);
  auto* env = new (std::nothrow) napi_env__(context, module_api_version);
  if (env == nullptr) return napi_generic_failure;
  {
    std::lock_guard<std::mutex> lock(g_runtime_mu);
    g_env_by_isolate[env->isolate] = env;
  }
  *result = env;
  return napi_ok;
}

napi_status NAPI_CDECL unofficial_napi_destroy_env_instance(napi_env env) {
  if (env == nullptr) return napi_invalid_arg;
  {
    std::lock_guard<std::mutex> lock(g_runtime_mu);
    auto env_it = g_env_by_isolate.find(env->isolate);
    if (env_it != g_env_by_isolate.end() && env_it->second == env) {
      g_env_by_isolate.erase(env_it);
    }
    auto cb_it = g_promise_reject_callbacks.find(env->isolate);
    if (cb_it != g_promise_reject_callbacks.end()) {
      cb_it->second.Reset();
      g_promise_reject_callbacks.erase(cb_it);
    }
  }
  if (env->isolate != nullptr) {
    env->isolate->SetPromiseRejectCallback(nullptr);
  }
  delete env;
  return napi_ok;
}

napi_status NAPI_CDECL unofficial_napi_wrap_existing_value(napi_env env,
                                                           v8::Local<v8::Value> value,
                                                           napi_value* result) {
  if (env == nullptr || value.IsEmpty() || result == nullptr) return napi_invalid_arg;
  *result = napi_v8_wrap_value(env, value);
  return (*result == nullptr) ? napi_generic_failure : napi_ok;
}

napi_status NAPI_CDECL unofficial_napi_create_env(int32_t module_api_version,
                                                  napi_env* env_out,
                                                  void** scope_out) {
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
  status = unofficial_napi_create_env_from_context(context, module_api_version, &scope->env);
  if (status != napi_ok || scope->env == nullptr) {
    delete scope;
    ReleaseRuntime();
    return (status == napi_ok) ? napi_generic_failure : status;
  }

  *env_out = scope->env;
  *scope_out = scope;
  return napi_ok;
}

napi_status NAPI_CDECL unofficial_napi_release_env(void* scope_ptr) {
  if (scope_ptr == nullptr) return napi_invalid_arg;
  auto* scope = static_cast<UnofficialEnvScope*>(scope_ptr);

  napi_status status = napi_ok;
  if (scope->env != nullptr) {
    status = unofficial_napi_destroy_env_instance(scope->env);
    scope->env = nullptr;
  }
  delete scope;
  ReleaseRuntime();
  return status;
}

napi_status NAPI_CDECL unofficial_napi_request_gc_for_testing(napi_env env) {
  if (env == nullptr || env->isolate == nullptr) return napi_invalid_arg;
  env->isolate->RequestGarbageCollectionForTesting(
      v8::Isolate::GarbageCollectionType::kFullGarbageCollection);
  env->isolate->PerformMicrotaskCheckpoint();
  return napi_ok;
}

napi_status NAPI_CDECL unofficial_napi_process_microtasks(napi_env env) {
  if (env == nullptr || env->isolate == nullptr) return napi_invalid_arg;
  // Keep this helper engine-agnostic in behavior: flush microtasks only.
  // Foreground task pumping is owned by higher-level runtime loop policy.
  env->isolate->PerformMicrotaskCheckpoint();
  return napi_ok;
}

napi_status NAPI_CDECL unofficial_napi_enqueue_microtask(napi_env env, napi_value callback) {
  if (env == nullptr || env->isolate == nullptr || callback == nullptr) return napi_invalid_arg;
  v8::Local<v8::Value> raw = napi_v8_unwrap_value(callback);
  if (!raw->IsFunction()) return napi_function_expected;
  env->context()->GetMicrotaskQueue()->EnqueueMicrotask(env->isolate, raw.As<v8::Function>());
  return napi_ok;
}

napi_status NAPI_CDECL unofficial_napi_set_promise_reject_callback(napi_env env,
                                                                   napi_value callback) {
  if (env == nullptr || env->isolate == nullptr || callback == nullptr) return napi_invalid_arg;
  v8::Local<v8::Value> raw = napi_v8_unwrap_value(callback);
  if (!raw->IsFunction()) return napi_function_expected;
  {
    std::lock_guard<std::mutex> lock(g_runtime_mu);
    g_env_by_isolate[env->isolate] = env;
    auto& slot = g_promise_reject_callbacks[env->isolate];
    slot.Reset();
    slot.Reset(env->isolate, raw.As<v8::Function>());
  }
  env->isolate->SetPromiseRejectCallback(PromiseRejectCallback);
  return napi_ok;
}

napi_status NAPI_CDECL unofficial_napi_get_promise_details(napi_env env,
                                                           napi_value promise,
                                                           int32_t* state_out,
                                                           napi_value* result_out,
                                                           bool* has_result_out) {
  if (env == nullptr || promise == nullptr || state_out == nullptr) return napi_invalid_arg;
  v8::Local<v8::Value> raw = napi_v8_unwrap_value(promise);
  if (raw.IsEmpty() || !raw->IsPromise()) return napi_invalid_arg;

  v8::Local<v8::Promise> p = raw.As<v8::Promise>();
  const v8::Promise::PromiseState state = p->State();
  *state_out = static_cast<int32_t>(state);

  const bool has_result = state != v8::Promise::PromiseState::kPending;
  if (has_result_out != nullptr) *has_result_out = has_result;

  if (result_out != nullptr) {
    *result_out = nullptr;
    if (has_result) {
      *result_out = napi_v8_wrap_value(env, p->Result());
      if (*result_out == nullptr) return napi_generic_failure;
    }
  }

  return napi_ok;
}

napi_status NAPI_CDECL unofficial_napi_get_proxy_details(napi_env env,
                                                         napi_value proxy,
                                                         napi_value* target_out,
                                                         napi_value* handler_out) {
  if (env == nullptr || proxy == nullptr || target_out == nullptr || handler_out == nullptr) {
    return napi_invalid_arg;
  }

  v8::Local<v8::Value> raw = napi_v8_unwrap_value(proxy);
  if (raw.IsEmpty() || !raw->IsProxy()) return napi_invalid_arg;

  v8::Local<v8::Proxy> p = raw.As<v8::Proxy>();
  *target_out = napi_v8_wrap_value(env, p->GetTarget());
  *handler_out = napi_v8_wrap_value(env, p->GetHandler());
  if (*target_out == nullptr || *handler_out == nullptr) return napi_generic_failure;
  return napi_ok;
}

napi_status NAPI_CDECL unofficial_napi_preview_entries(napi_env env,
                                                       napi_value value,
                                                       napi_value* entries_out,
                                                       bool* is_key_value_out) {
  if (env == nullptr || value == nullptr || entries_out == nullptr || is_key_value_out == nullptr) {
    return napi_invalid_arg;
  }

  v8::Local<v8::Value> raw = napi_v8_unwrap_value(value);
  if (raw.IsEmpty() || !raw->IsObject()) return napi_invalid_arg;

  bool is_key_value = false;
  v8::Local<v8::Array> entries;
  if (!raw.As<v8::Object>()->PreviewEntries(&is_key_value).ToLocal(&entries)) {
    return napi_generic_failure;
  }
  *entries_out = napi_v8_wrap_value(env, entries);
  if (*entries_out == nullptr) return napi_generic_failure;
  *is_key_value_out = is_key_value;
  return napi_ok;
}

napi_status NAPI_CDECL unofficial_napi_get_call_sites(napi_env env,
                                                      uint32_t frames,
                                                      napi_value* callsites_out) {
  if (env == nullptr || env->isolate == nullptr || callsites_out == nullptr) return napi_invalid_arg;
  if (frames < 1 || frames > 200) return napi_invalid_arg;

  v8::Isolate* isolate = env->isolate;
  v8::HandleScope scope(isolate);
  v8::Local<v8::Context> context = env->context();

  v8::Local<v8::StackTrace> stack = v8::StackTrace::CurrentStackTrace(isolate, frames + 1);
  const int frame_count = stack->GetFrameCount();
  int start_index = 0;
  if (frame_count > 0) {
    v8::Local<v8::StackFrame> first_frame = stack->GetFrame(isolate, 0);
    v8::Local<v8::Value> first_script_name = first_frame->GetScriptName();
    if (!first_script_name.IsEmpty() && first_script_name->IsString()) {
      v8::String::Utf8Value first_script_name_utf8(isolate, first_script_name);
      if (*first_script_name_utf8 != nullptr) {
        std::string script_name(*first_script_name_utf8,
                                static_cast<size_t>(first_script_name_utf8.length()));
        if (script_name.rfind("node:util", 0) == 0) {
          start_index = 1;
        }
      }
    }
  }
  const int available = frame_count - start_index;
  uint32_t count = available > 0 ? static_cast<uint32_t>(available) : 0;
  if (count > frames) count = frames;
  v8::Local<v8::Array> out = v8::Array::New(isolate, count);

  auto set_named = [&](v8::Local<v8::Object> obj, const char* key, v8::Local<v8::Value> value) -> bool {
    v8::Local<v8::String> k;
    if (!v8::String::NewFromUtf8(isolate, key, v8::NewStringType::kNormal).ToLocal(&k)) return false;
    return obj->Set(context, k, value).FromMaybe(false);
  };

  for (uint32_t out_index = 0; out_index < count; ++out_index) {
    const int i = start_index + static_cast<int>(out_index);
    v8::Local<v8::StackFrame> frame = stack->GetFrame(isolate, i);
    v8::Local<v8::Object> callsite = v8::Object::New(isolate);
    if (callsite->SetPrototype(context, v8::Null(isolate)).IsNothing()) return napi_generic_failure;

    v8::Local<v8::Value> function_name = frame->GetFunctionName();
    if (function_name.IsEmpty()) function_name = v8::String::Empty(isolate);

    v8::Local<v8::Value> script_name = frame->GetScriptName();
    if (script_name.IsEmpty()) script_name = v8::String::Empty(isolate);
    v8::Local<v8::Value> script_name_or_source_url = frame->GetScriptNameOrSourceURL();
    if (script_name_or_source_url.IsEmpty()) script_name_or_source_url = v8::String::Empty(isolate);

    const std::string script_id = std::to_string(frame->GetScriptId());
    v8::Local<v8::String> script_id_v8;
    if (!v8::String::NewFromUtf8(isolate,
                                 script_id.data(),
                                 v8::NewStringType::kNormal,
                                 static_cast<int>(script_id.size()))
             .ToLocal(&script_id_v8)) {
      return napi_generic_failure;
    }

    const uint32_t line = frame->GetLineNumber();
    const uint32_t col = frame->GetColumn();
    if (!set_named(callsite, "functionName", function_name) ||
        !set_named(callsite, "scriptId", script_id_v8) ||
        !set_named(callsite, "scriptName", script_name) ||
        !set_named(callsite, "scriptNameOrSourceURL", script_name_or_source_url) ||
        !set_named(callsite, "lineNumber", v8::Integer::NewFromUnsigned(isolate, line)) ||
        !set_named(callsite, "columnNumber", v8::Integer::NewFromUnsigned(isolate, col)) ||
        !set_named(callsite, "column", v8::Integer::NewFromUnsigned(isolate, col)) ||
        out->Set(context, out_index, callsite).IsNothing()) {
      return napi_generic_failure;
    }
  }

  *callsites_out = napi_v8_wrap_value(env, out);
  if (*callsites_out == nullptr) return napi_generic_failure;
  return napi_ok;
}

napi_status NAPI_CDECL unofficial_napi_arraybuffer_view_has_buffer(napi_env env,
                                                                   napi_value value,
                                                                   bool* result_out) {
  if (env == nullptr || value == nullptr || result_out == nullptr) return napi_invalid_arg;
  v8::Local<v8::Value> raw = napi_v8_unwrap_value(value);
  if (raw.IsEmpty() || !raw->IsArrayBufferView()) return napi_invalid_arg;
  *result_out = raw.As<v8::ArrayBufferView>()->HasBuffer();
  return napi_ok;
}

napi_status NAPI_CDECL unofficial_napi_get_constructor_name(napi_env env,
                                                            napi_value value,
                                                            napi_value* name_out) {
  if (env == nullptr || value == nullptr || name_out == nullptr) return napi_invalid_arg;
  v8::Local<v8::Value> raw = napi_v8_unwrap_value(value);
  if (raw.IsEmpty() || !raw->IsObject()) return napi_invalid_arg;
  v8::Local<v8::String> name = raw.As<v8::Object>()->GetConstructorName();
  *name_out = napi_v8_wrap_value(env, name);
  return *name_out == nullptr ? napi_generic_failure : napi_ok;
}

napi_status NAPI_CDECL unofficial_napi_notify_datetime_configuration_change(napi_env env) {
  if (env == nullptr || env->isolate == nullptr) return napi_invalid_arg;
#if defined(__POSIX__)
  tzset();
#elif defined(_WIN32)
  _tzset();
#endif
  env->isolate->DateTimeConfigurationChangeNotification(
      v8::Isolate::TimeZoneDetection::kRedetect);
  return napi_ok;
}

}  // extern "C"
