#include "unofficial_napi.h"

#include <atomic>
#include <ctime>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <unordered_map>
#include <vector>

#include <libplatform/libplatform.h>

#include "internal/napi_v8_env.h"
#include "internal/unofficial_napi_bridge.h"
#include "ubi_v8_platform.h"

namespace {

struct SharedRuntime {
  std::unique_ptr<UbiV8Platform> platform;
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
std::unordered_map<v8::ArrayBuffer::Allocator*, void*> g_tracking_allocators;

class TrackingArrayBufferAllocator final : public v8::ArrayBuffer::Allocator {
 public:
  TrackingArrayBufferAllocator()
      : backing_(v8::ArrayBuffer::Allocator::NewDefaultAllocator()) {}

  ~TrackingArrayBufferAllocator() override { delete backing_; }

  void* Allocate(size_t length) override {
    void* data = backing_ != nullptr ? backing_->Allocate(length) : nullptr;
    if (data != nullptr) {
      total_mem_usage_.fetch_add(length, std::memory_order_relaxed);
    }
    return data;
  }

  void* AllocateUninitialized(size_t length) override {
    void* data = backing_ != nullptr ? backing_->AllocateUninitialized(length) : nullptr;
    if (data != nullptr) {
      total_mem_usage_.fetch_add(length, std::memory_order_relaxed);
    }
    return data;
  }

  void Free(void* data, size_t length) override {
    if (data != nullptr) {
      total_mem_usage_.fetch_sub(length, std::memory_order_relaxed);
    }
    if (backing_ != nullptr) {
      backing_->Free(data, length);
    }
  }

  size_t MaxAllocationSize() const override {
    return backing_ != nullptr ? backing_->MaxAllocationSize()
                               : v8::ArrayBuffer::Allocator::MaxAllocationSize();
  }

  v8::PageAllocator* GetPageAllocator() override {
    return backing_ != nullptr ? backing_->GetPageAllocator() : nullptr;
  }

  uint64_t total_mem_usage() const {
    return total_mem_usage_.load(std::memory_order_relaxed);
  }

 private:
  v8::ArrayBuffer::Allocator* backing_ = nullptr;
  std::atomic<uint64_t> total_mem_usage_ {0};
};

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
    g_runtime.platform = UbiV8Platform::Create();
    v8::V8::InitializePlatform(g_runtime.platform.get());
    v8::V8::Initialize();

    auto* allocator = new TrackingArrayBufferAllocator();
    g_runtime.params.array_buffer_allocator = allocator;
    g_tracking_allocators[g_runtime.params.array_buffer_allocator] = allocator;
    g_runtime.isolate = v8::Isolate::New(g_runtime.params);
    if (g_runtime.isolate == nullptr) {
      delete g_runtime.params.array_buffer_allocator;
      g_runtime.params.array_buffer_allocator = nullptr;
      g_tracking_allocators.erase(allocator);
      g_runtime.platform.reset();
      v8::V8::Dispose();
      v8::V8::DisposePlatform();
      return napi_generic_failure;
    }
    if (!g_runtime.platform->RegisterIsolate(g_runtime.isolate)) {
      g_runtime.isolate->Dispose();
      g_runtime.isolate = nullptr;
      delete g_runtime.params.array_buffer_allocator;
      g_runtime.params.array_buffer_allocator = nullptr;
      g_tracking_allocators.erase(allocator);
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

v8::Local<v8::String> OneByteString(v8::Isolate* isolate, const char* value) {
  return v8::String::NewFromUtf8(isolate, value, v8::NewStringType::kInternalized)
      .ToLocalChecked();
}

bool ReadArrayBufferViewBytes(v8::Local<v8::Value> value,
                              const uint8_t** data_out,
                              size_t* size_out) {
  if (data_out == nullptr || size_out == nullptr || value.IsEmpty() || !value->IsArrayBufferView()) {
    return false;
  }
  v8::Local<v8::ArrayBufferView> view = value.As<v8::ArrayBufferView>();
  std::shared_ptr<v8::BackingStore> store = view->Buffer()->GetBackingStore();
  if (!store || store->Data() == nullptr) {
    *data_out = nullptr;
    *size_out = 0;
    return true;
  }
  *data_out = static_cast<const uint8_t*>(store->Data()) + view->ByteOffset();
  *size_out = view->ByteLength();
  return true;
}

class SerializerContext {
 public:
  SerializerContext(napi_env env, v8::Local<v8::Object> wrap)
      : env_(env), isolate_(env->isolate), serializer_(isolate_) {
    wrap_.Reset(isolate_, wrap);
    wrap_.SetWeak(this, WeakCallback, v8::WeakCallbackType::kParameter);
  }

  ~SerializerContext() { wrap_.Reset(); }

  static void New(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* isolate = args.GetIsolate();
    if (!args.IsConstructCall()) {
      isolate->ThrowException(v8::Exception::TypeError(
          OneByteString(isolate, "Class constructor Serializer cannot be invoked without 'new'")));
      return;
    }
    if (!args.Data()->IsExternal()) {
      isolate->ThrowException(v8::Exception::Error(
          OneByteString(isolate, "Internal serializer constructor state missing")));
      return;
    }

    auto* env = static_cast<napi_env>(v8::Local<v8::External>::Cast(args.Data())->Value());
    if (env == nullptr) {
      isolate->ThrowException(v8::Exception::Error(
          OneByteString(isolate, "Internal serializer environment missing")));
      return;
    }

    auto* ctx = new SerializerContext(env, args.This());
    args.This()->SetAlignedPointerInInternalField(0, ctx);
  }

  static void WriteHeader(const v8::FunctionCallbackInfo<v8::Value>& args) {
    SerializerContext* ctx = Unwrap(args);
    if (ctx == nullptr) return;
    ctx->serializer_.WriteHeader();
  }

  static void WriteValue(const v8::FunctionCallbackInfo<v8::Value>& args) {
    SerializerContext* ctx = Unwrap(args);
    if (ctx == nullptr) return;
    v8::Local<v8::Value> value = args.Length() >= 1 ? args[0] : v8::Undefined(ctx->isolate_);
    bool ret = false;
    if (ctx->serializer_.WriteValue(ctx->env_->context(), value).To(&ret)) {
      args.GetReturnValue().Set(ret);
    }
  }

  static void SetTreatArrayBufferViewsAsHostObjects(
      const v8::FunctionCallbackInfo<v8::Value>& args) {
    SerializerContext* ctx = Unwrap(args);
    if (ctx == nullptr) return;
    (void)args;
    // Intentionally a no-op: keep V8 default ArrayBufferView serialization.
  }

  static void ReleaseBuffer(const v8::FunctionCallbackInfo<v8::Value>& args) {
    SerializerContext* ctx = Unwrap(args);
    if (ctx == nullptr) return;

    std::pair<uint8_t*, size_t> serialized = ctx->serializer_.Release();
    v8::Isolate* isolate = ctx->isolate_;
    v8::HandleScope handle_scope(isolate);
    v8::Local<v8::Context> context = ctx->env_->context();
    v8::Context::Scope context_scope(context);

    auto backing_store = v8::ArrayBuffer::NewBackingStore(
        serialized.first,
        serialized.second,
        [](void* data, size_t, void*) { std::free(data); },
        nullptr);
    if (!backing_store) {
      std::free(serialized.first);
      return;
    }
    v8::Local<v8::ArrayBuffer> ab =
        v8::ArrayBuffer::New(isolate, std::move(backing_store));
    v8::Local<v8::Uint8Array> view =
        v8::Uint8Array::New(ab, 0, serialized.second);

    v8::Local<v8::Value> buffer_ctor;
    if (context->Global()->Get(context, OneByteString(isolate, "Buffer")).ToLocal(&buffer_ctor) &&
        buffer_ctor->IsFunction()) {
      v8::Local<v8::Value> from_val;
      if (buffer_ctor.As<v8::Object>()->Get(context, OneByteString(isolate, "from")).ToLocal(&from_val) &&
          from_val->IsFunction()) {
        v8::Local<v8::Value> argv[1] = {view};
        v8::Local<v8::Value> out;
        if (from_val.As<v8::Function>()->Call(context, buffer_ctor, 1, argv).ToLocal(&out)) {
          args.GetReturnValue().Set(out);
          return;
        }
      }
    }

    args.GetReturnValue().Set(view);
  }

  static void TransferArrayBuffer(const v8::FunctionCallbackInfo<v8::Value>& args) {
    SerializerContext* ctx = Unwrap(args);
    if (ctx == nullptr) return;
    if (args.Length() < 2) return;

    uint32_t id = 0;
    if (!args[0]->Uint32Value(ctx->env_->context()).To(&id)) return;

    if (!args[1]->IsArrayBuffer()) {
      ctx->isolate_->ThrowException(v8::Exception::TypeError(
          OneByteString(ctx->isolate_, "arrayBuffer must be an ArrayBuffer")));
      return;
    }
    ctx->serializer_.TransferArrayBuffer(id, args[1].As<v8::ArrayBuffer>());
  }

  static void WriteUint32(const v8::FunctionCallbackInfo<v8::Value>& args) {
    SerializerContext* ctx = Unwrap(args);
    if (ctx == nullptr || args.Length() < 1) return;
    uint32_t value = 0;
    if (args[0]->Uint32Value(ctx->env_->context()).To(&value)) {
      ctx->serializer_.WriteUint32(value);
    }
  }

  static void WriteUint64(const v8::FunctionCallbackInfo<v8::Value>& args) {
    SerializerContext* ctx = Unwrap(args);
    if (ctx == nullptr || args.Length() < 2) return;
    uint32_t hi = 0;
    uint32_t lo = 0;
    if (!args[0]->Uint32Value(ctx->env_->context()).To(&hi) ||
        !args[1]->Uint32Value(ctx->env_->context()).To(&lo)) {
      return;
    }
    ctx->serializer_.WriteUint64((static_cast<uint64_t>(hi) << 32) | static_cast<uint64_t>(lo));
  }

  static void WriteDouble(const v8::FunctionCallbackInfo<v8::Value>& args) {
    SerializerContext* ctx = Unwrap(args);
    if (ctx == nullptr || args.Length() < 1) return;
    double value = 0;
    if (args[0]->NumberValue(ctx->env_->context()).To(&value)) {
      ctx->serializer_.WriteDouble(value);
    }
  }

  static void WriteRawBytes(const v8::FunctionCallbackInfo<v8::Value>& args) {
    SerializerContext* ctx = Unwrap(args);
    if (ctx == nullptr || args.Length() < 1) return;
    const uint8_t* data = nullptr;
    size_t size = 0;
    if (!ReadArrayBufferViewBytes(args[0], &data, &size)) {
      ctx->isolate_->ThrowException(v8::Exception::TypeError(
          OneByteString(ctx->isolate_, "source must be a TypedArray or a DataView")));
      return;
    }
    ctx->serializer_.WriteRawBytes(data, size);
  }

 private:
  static SerializerContext* Unwrap(const v8::FunctionCallbackInfo<v8::Value>& args) {
    if (args.This().IsEmpty() || args.This()->InternalFieldCount() < 1) return nullptr;
    return static_cast<SerializerContext*>(args.This()->GetAlignedPointerFromInternalField(0));
  }

  static void WeakCallback(const v8::WeakCallbackInfo<SerializerContext>& info) {
    delete info.GetParameter();
  }

  napi_env env_;
  v8::Isolate* isolate_;
  v8::Global<v8::Object> wrap_;
  v8::ValueSerializer serializer_;
};

class DeserializerContext {
 public:
  DeserializerContext(napi_env env,
                      v8::Local<v8::Object> wrap,
                      v8::Local<v8::Value> buffer)
      : env_(env), isolate_(env->isolate) {
    const uint8_t* data = nullptr;
    size_t size = 0;
    if (!ReadArrayBufferViewBytes(buffer, &data, &size)) return;
    if (data != nullptr && size > 0) {
      data_.assign(data, data + size);
    }
    deserializer_ = std::make_unique<v8::ValueDeserializer>(
        isolate_, data_.empty() ? nullptr : data_.data(), data_.size());

    wrap_.Reset(isolate_, wrap);
    wrap_.SetWeak(this, WeakCallback, v8::WeakCallbackType::kParameter);

    v8::Local<v8::Context> context = env_->context();
    v8::Context::Scope context_scope(context);
    (void)wrap->Set(context, OneByteString(isolate_, "buffer"), buffer);
  }

  ~DeserializerContext() {
    wrap_.Reset();
    deserializer_.reset();
  }

  static void New(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* isolate = args.GetIsolate();
    if (!args.IsConstructCall()) {
      isolate->ThrowException(v8::Exception::TypeError(
          OneByteString(isolate, "Class constructor Deserializer cannot be invoked without 'new'")));
      return;
    }
    if (!args.Data()->IsExternal()) {
      isolate->ThrowException(v8::Exception::Error(
          OneByteString(isolate, "Internal deserializer constructor state missing")));
      return;
    }
    if (args.Length() < 1 || !args[0]->IsArrayBufferView()) {
      isolate->ThrowException(v8::Exception::TypeError(
          OneByteString(isolate, "buffer must be a TypedArray or a DataView")));
      return;
    }

    auto* env = static_cast<napi_env>(v8::Local<v8::External>::Cast(args.Data())->Value());
    if (env == nullptr) {
      isolate->ThrowException(v8::Exception::Error(
          OneByteString(isolate, "Internal deserializer environment missing")));
      return;
    }
    auto* ctx = new DeserializerContext(env, args.This(), args[0]);
    args.This()->SetAlignedPointerInInternalField(0, ctx);
  }

  static void ReadHeader(const v8::FunctionCallbackInfo<v8::Value>& args) {
    DeserializerContext* ctx = Unwrap(args);
    if (ctx == nullptr || !ctx->deserializer_) return;
    bool ok = false;
    if (ctx->deserializer_->ReadHeader(ctx->env_->context()).To(&ok)) {
      args.GetReturnValue().Set(ok);
    }
  }

  static void ReadValue(const v8::FunctionCallbackInfo<v8::Value>& args) {
    DeserializerContext* ctx = Unwrap(args);
    if (ctx == nullptr || !ctx->deserializer_) return;
    v8::Local<v8::Value> out;
    if (ctx->deserializer_->ReadValue(ctx->env_->context()).ToLocal(&out)) {
      args.GetReturnValue().Set(out);
    }
  }

  static void TransferArrayBuffer(const v8::FunctionCallbackInfo<v8::Value>& args) {
    DeserializerContext* ctx = Unwrap(args);
    if (ctx == nullptr || !ctx->deserializer_ || args.Length() < 2) return;
    uint32_t id = 0;
    if (!args[0]->Uint32Value(ctx->env_->context()).To(&id)) return;
    if (!args[1]->IsArrayBuffer()) {
      ctx->isolate_->ThrowException(v8::Exception::TypeError(
          OneByteString(ctx->isolate_, "arrayBuffer must be an ArrayBuffer")));
      return;
    }
    ctx->deserializer_->TransferArrayBuffer(id, args[1].As<v8::ArrayBuffer>());
  }

  static void GetWireFormatVersion(const v8::FunctionCallbackInfo<v8::Value>& args) {
    DeserializerContext* ctx = Unwrap(args);
    if (ctx == nullptr || !ctx->deserializer_) return;
    args.GetReturnValue().Set(v8::Integer::NewFromUnsigned(
        ctx->isolate_, ctx->deserializer_->GetWireFormatVersion()));
  }

  static void ReadUint32(const v8::FunctionCallbackInfo<v8::Value>& args) {
    DeserializerContext* ctx = Unwrap(args);
    if (ctx == nullptr || !ctx->deserializer_) return;
    uint32_t value = 0;
    if (!ctx->deserializer_->ReadUint32(&value)) {
      ctx->isolate_->ThrowException(v8::Exception::Error(
          OneByteString(ctx->isolate_, "ReadUint32() failed")));
      return;
    }
    args.GetReturnValue().Set(v8::Integer::NewFromUnsigned(ctx->isolate_, value));
  }

  static void ReadUint64(const v8::FunctionCallbackInfo<v8::Value>& args) {
    DeserializerContext* ctx = Unwrap(args);
    if (ctx == nullptr || !ctx->deserializer_) return;
    uint64_t value = 0;
    if (!ctx->deserializer_->ReadUint64(&value)) {
      ctx->isolate_->ThrowException(v8::Exception::Error(
          OneByteString(ctx->isolate_, "ReadUint64() failed")));
      return;
    }
    const uint32_t hi = static_cast<uint32_t>(value >> 32);
    const uint32_t lo = static_cast<uint32_t>(value);
    v8::Local<v8::Value> vals[2] = {
        v8::Integer::NewFromUnsigned(ctx->isolate_, hi),
        v8::Integer::NewFromUnsigned(ctx->isolate_, lo),
    };
    args.GetReturnValue().Set(v8::Array::New(ctx->isolate_, vals, 2));
  }

  static void ReadDouble(const v8::FunctionCallbackInfo<v8::Value>& args) {
    DeserializerContext* ctx = Unwrap(args);
    if (ctx == nullptr || !ctx->deserializer_) return;
    double value = 0;
    if (!ctx->deserializer_->ReadDouble(&value)) {
      ctx->isolate_->ThrowException(v8::Exception::Error(
          OneByteString(ctx->isolate_, "ReadDouble() failed")));
      return;
    }
    args.GetReturnValue().Set(value);
  }

  static void ReadRawBytes(const v8::FunctionCallbackInfo<v8::Value>& args) {
    DeserializerContext* ctx = Unwrap(args);
    if (ctx == nullptr || !ctx->deserializer_ || args.Length() < 1) return;

    int64_t requested = 0;
    if (!args[0]->IntegerValue(ctx->env_->context()).To(&requested) || requested < 0) {
      return;
    }
    const size_t length = static_cast<size_t>(requested);

    const void* read_ptr = nullptr;
    if (!ctx->deserializer_->ReadRawBytes(length, &read_ptr)) {
      ctx->isolate_->ThrowException(v8::Exception::Error(
          OneByteString(ctx->isolate_, "ReadRawBytes() failed")));
      return;
    }
    const uint8_t* pos = static_cast<const uint8_t*>(read_ptr);
    const uint8_t* base = ctx->data_.empty() ? nullptr : ctx->data_.data();
    if (base == nullptr || pos < base || pos + length > base + ctx->data_.size()) {
      ctx->isolate_->ThrowException(v8::Exception::Error(
          OneByteString(ctx->isolate_, "ReadRawBytes() returned out-of-range data")));
      return;
    }
    const uint32_t offset = static_cast<uint32_t>(pos - base);
    args.GetReturnValue().Set(v8::Integer::NewFromUnsigned(ctx->isolate_, offset));
  }

 private:
  static DeserializerContext* Unwrap(const v8::FunctionCallbackInfo<v8::Value>& args) {
    if (args.This().IsEmpty() || args.This()->InternalFieldCount() < 1) return nullptr;
    return static_cast<DeserializerContext*>(args.This()->GetAlignedPointerFromInternalField(0));
  }

  static void WeakCallback(const v8::WeakCallbackInfo<DeserializerContext>& info) {
    delete info.GetParameter();
  }

  napi_env env_;
  v8::Isolate* isolate_;
  v8::Global<v8::Object> wrap_;
  std::vector<uint8_t> data_;
  std::unique_ptr<v8::ValueDeserializer> deserializer_;
};

void SetProtoMethod(v8::Isolate* isolate,
                    v8::Local<v8::FunctionTemplate> tmpl,
                    const char* name,
                    v8::FunctionCallback callback) {
  tmpl->PrototypeTemplate()->Set(
      isolate,
      name,
      v8::FunctionTemplate::New(isolate, callback));
}

bool SetConstructorFunction(v8::Local<v8::Context> context,
                            v8::Local<v8::Object> target,
                            const char* name,
                            v8::Local<v8::FunctionTemplate> tmpl) {
  v8::Local<v8::Function> ctor;
  if (!tmpl->GetFunction(context).ToLocal(&ctor)) return false;
  return target->Set(context, OneByteString(context->GetIsolate(), name), ctor).FromMaybe(false);
}

}  // namespace

extern "C" {

napi_status NAPI_CDECL unofficial_napi_set_enqueue_foreground_task_callback(
    napi_env env,
    unofficial_napi_enqueue_foreground_task_callback callback) {
  if (env == nullptr) return napi_invalid_arg;
  env->enqueue_foreground_task_callback = callback;
  return napi_ok;
}

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

void DrainMicrotasksForEnv(napi_env env) {
  if (env == nullptr || env->isolate == nullptr) return;
  v8::Local<v8::Context> context = env->context();
  if (!context.IsEmpty()) {
    v8::MicrotaskQueue* queue = context->GetMicrotaskQueue();
    if (queue != nullptr) {
      queue->PerformCheckpoint(env->isolate);
      return;
    }
  }
  env->isolate->PerformMicrotaskCheckpoint();
}

napi_status NAPI_CDECL unofficial_napi_request_gc_for_testing(napi_env env) {
  if (env == nullptr || env->isolate == nullptr) return napi_invalid_arg;
  // Match Node test expectations for global.gc(): force an actual full GC
  // cycle rather than only hinting memory pressure.
  env->isolate->RequestGarbageCollectionForTesting(
      v8::Isolate::GarbageCollectionType::kFullGarbageCollection);
  return napi_ok;
}

napi_status NAPI_CDECL unofficial_napi_process_microtasks(napi_env env) {
  if (env == nullptr || env->isolate == nullptr) return napi_invalid_arg;
  // Keep this helper scoped to the current context's microtask queue.
  // Foreground task pumping is owned by higher-level runtime loop policy.
  DrainMicrotasksForEnv(env);
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
  const int start_index = frame_count > 0 ? 1 : 0;
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

napi_status NAPI_CDECL unofficial_napi_get_caller_location(napi_env env, napi_value* location_out) {
  if (env == nullptr || env->isolate == nullptr || location_out == nullptr) return napi_invalid_arg;
  *location_out = nullptr;

  v8::Isolate* isolate = env->isolate;
  v8::HandleScope scope(isolate);

  v8::Local<v8::StackTrace> trace = v8::StackTrace::CurrentStackTrace(isolate, 2);
  if (trace->GetFrameCount() != 2) {
    return napi_ok;
  }

  v8::Local<v8::StackFrame> frame = trace->GetFrame(isolate, 1);
  v8::Local<v8::Value> file = frame->GetScriptNameOrSourceURL();
  if (file.IsEmpty()) {
    return napi_ok;
  }

  v8::Local<v8::Value> values[] = {
      v8::Integer::New(isolate, frame->GetLineNumber()),
      v8::Integer::New(isolate, frame->GetColumn()),
      file,
  };
  v8::Local<v8::Array> location = v8::Array::New(isolate, values, 3);
  *location_out = napi_v8_wrap_value(env, location);
  return *location_out == nullptr ? napi_generic_failure : napi_ok;
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

napi_status NAPI_CDECL unofficial_napi_create_private_symbol(napi_env env,
                                                             const char* utf8description,
                                                             size_t length,
                                                             napi_value* result_out) {
  if (env == nullptr || env->isolate == nullptr || result_out == nullptr) return napi_invalid_arg;
  if (utf8description == nullptr && length > 0) return napi_invalid_arg;

  v8::Isolate* isolate = env->isolate;
  v8::HandleScope scope(isolate);
  v8::Local<v8::Context> context = env->context();

  const char* description = utf8description != nullptr ? utf8description : "";
  const int v8_length = (length == NAPI_AUTO_LENGTH) ? -1 : static_cast<int>(length);
  v8::Local<v8::String> desc;
  if (!v8::String::NewFromUtf8(isolate, description, v8::NewStringType::kInternalized, v8_length)
           .ToLocal(&desc)) {
    return napi_generic_failure;
  }

  v8::Local<v8::Private> priv = v8::Private::ForApi(isolate, desc);
  v8::Local<v8::ObjectTemplate> tmpl = v8::ObjectTemplate::New(isolate);
  tmpl->Set(v8::String::NewFromUtf8Literal(isolate, "value"), priv);

  v8::Local<v8::Object> holder;
  if (!tmpl->NewInstance(context).ToLocal(&holder)) {
    return napi_generic_failure;
  }

  v8::Local<v8::Value> symbol_value;
  if (!holder->Get(context, v8::String::NewFromUtf8Literal(isolate, "value")).ToLocal(&symbol_value)) {
    return napi_generic_failure;
  }

  *result_out = napi_v8_wrap_value(env, symbol_value);
  return *result_out == nullptr ? napi_generic_failure : napi_ok;
}

napi_status NAPI_CDECL unofficial_napi_get_process_memory_info(
    napi_env env,
    double* heap_total_out,
    double* heap_used_out,
    double* external_out,
    double* array_buffers_out) {
  if (env == nullptr || env->isolate == nullptr || heap_total_out == nullptr ||
      heap_used_out == nullptr || external_out == nullptr || array_buffers_out == nullptr) {
    return napi_invalid_arg;
  }

  v8::HeapStatistics stats;
  env->isolate->GetHeapStatistics(&stats);

  uint64_t array_buffers = 0;
  {
    std::lock_guard<std::mutex> lock(g_runtime_mu);
    auto it = g_tracking_allocators.find(env->isolate->GetArrayBufferAllocator());
    if (it != g_tracking_allocators.end() && it->second != nullptr) {
      auto* allocator = static_cast<TrackingArrayBufferAllocator*>(it->second);
      array_buffers = allocator->total_mem_usage();
    }
  }

  *heap_total_out = static_cast<double>(stats.total_heap_size());
  *heap_used_out = static_cast<double>(stats.used_heap_size());
  *external_out = static_cast<double>(stats.external_memory());
  *array_buffers_out = static_cast<double>(array_buffers);
  return napi_ok;
}

napi_status NAPI_CDECL unofficial_napi_get_continuation_preserved_embedder_data(
    napi_env env,
    napi_value* result_out) {
  if (env == nullptr || env->isolate == nullptr || result_out == nullptr) return napi_invalid_arg;
  v8::Local<v8::Value> value = env->isolate->GetContinuationPreservedEmbedderData();
  if (value.IsEmpty()) value = v8::Undefined(env->isolate);
  *result_out = napi_v8_wrap_value(env, value);
  return *result_out == nullptr ? napi_generic_failure : napi_ok;
}

napi_status NAPI_CDECL unofficial_napi_set_continuation_preserved_embedder_data(
    napi_env env,
    napi_value value) {
  if (env == nullptr || env->isolate == nullptr) return napi_invalid_arg;
  v8::Local<v8::Value> raw =
      value != nullptr ? napi_v8_unwrap_value(value) : v8::Local<v8::Value>();
  if (raw.IsEmpty()) raw = v8::Undefined(env->isolate);
  env->isolate->SetContinuationPreservedEmbedderData(raw);
  return napi_ok;
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

napi_status NAPI_CDECL unofficial_napi_create_serdes_binding(napi_env env,
                                                             napi_value* result_out) {
  if (env == nullptr || env->isolate == nullptr || result_out == nullptr) return napi_invalid_arg;

  v8::Isolate* isolate = env->isolate;
  v8::HandleScope handle_scope(isolate);
  v8::Local<v8::Context> context = env->context();
  v8::Context::Scope context_scope(context);

  v8::Local<v8::Object> target = v8::Object::New(isolate);
  v8::Local<v8::External> env_data = v8::External::New(isolate, env);

  v8::Local<v8::FunctionTemplate> serializer_tmpl =
      v8::FunctionTemplate::New(isolate, SerializerContext::New, env_data);
  serializer_tmpl->InstanceTemplate()->SetInternalFieldCount(1);
  SetProtoMethod(isolate, serializer_tmpl, "writeHeader", SerializerContext::WriteHeader);
  SetProtoMethod(isolate, serializer_tmpl, "writeValue", SerializerContext::WriteValue);
  SetProtoMethod(isolate, serializer_tmpl, "releaseBuffer", SerializerContext::ReleaseBuffer);
  SetProtoMethod(isolate, serializer_tmpl, "transferArrayBuffer", SerializerContext::TransferArrayBuffer);
  SetProtoMethod(isolate, serializer_tmpl, "writeUint32", SerializerContext::WriteUint32);
  SetProtoMethod(isolate, serializer_tmpl, "writeUint64", SerializerContext::WriteUint64);
  SetProtoMethod(isolate, serializer_tmpl, "writeDouble", SerializerContext::WriteDouble);
  SetProtoMethod(isolate, serializer_tmpl, "writeRawBytes", SerializerContext::WriteRawBytes);
  SetProtoMethod(isolate,
                 serializer_tmpl,
                 "_setTreatArrayBufferViewsAsHostObjects",
                 SerializerContext::SetTreatArrayBufferViewsAsHostObjects);
  if (!SetConstructorFunction(context, target, "Serializer", serializer_tmpl)) {
    return napi_generic_failure;
  }

  v8::Local<v8::FunctionTemplate> deserializer_tmpl =
      v8::FunctionTemplate::New(isolate, DeserializerContext::New, env_data);
  deserializer_tmpl->InstanceTemplate()->SetInternalFieldCount(1);
  SetProtoMethod(isolate, deserializer_tmpl, "readHeader", DeserializerContext::ReadHeader);
  SetProtoMethod(isolate, deserializer_tmpl, "readValue", DeserializerContext::ReadValue);
  SetProtoMethod(
      isolate, deserializer_tmpl, "getWireFormatVersion", DeserializerContext::GetWireFormatVersion);
  SetProtoMethod(
      isolate, deserializer_tmpl, "transferArrayBuffer", DeserializerContext::TransferArrayBuffer);
  SetProtoMethod(isolate, deserializer_tmpl, "readUint32", DeserializerContext::ReadUint32);
  SetProtoMethod(isolate, deserializer_tmpl, "readUint64", DeserializerContext::ReadUint64);
  SetProtoMethod(isolate, deserializer_tmpl, "readDouble", DeserializerContext::ReadDouble);
  SetProtoMethod(isolate, deserializer_tmpl, "_readRawBytes", DeserializerContext::ReadRawBytes);
  if (!SetConstructorFunction(context, target, "Deserializer", deserializer_tmpl)) {
    return napi_generic_failure;
  }

  *result_out = napi_v8_wrap_value(env, target);
  return (*result_out == nullptr) ? napi_generic_failure : napi_ok;
}

}  // extern "C"

bool NapiV8LookupForegroundTaskTarget(
    v8::Isolate* isolate,
    napi_env* env_out,
    unofficial_napi_enqueue_foreground_task_callback* callback_out) {
  if (env_out != nullptr) *env_out = nullptr;
  if (callback_out != nullptr) *callback_out = nullptr;
  if (isolate == nullptr) return false;
  std::lock_guard<std::mutex> lock(g_runtime_mu);
  const auto it = g_env_by_isolate.find(isolate);
  if (it == g_env_by_isolate.end() || it->second == nullptr) return false;
  if (env_out != nullptr) *env_out = it->second;
  if (callback_out != nullptr) *callback_out = it->second->enqueue_foreground_task_callback;
  return true;
}
