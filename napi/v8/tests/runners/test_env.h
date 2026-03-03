#ifndef NAPI_V8_TEST_ENV_H_
#define NAPI_V8_TEST_ENV_H_

#include <memory>

#include <gtest/gtest.h>
#include <libplatform/libplatform.h>
#include <v8.h>

#include "unofficial_napi.h"

extern "C" {
napi_status NAPI_CDECL unofficial_napi_create_env_from_context(
    v8::Local<v8::Context> context, int32_t module_api_version, napi_env* result);
napi_status NAPI_CDECL unofficial_napi_destroy_env_instance(napi_env env);
}

class V8Runtime {
 public:
  V8Runtime() {
    static constexpr char kDefaultFlags[] = "--js-float16array";
    v8::V8::SetFlagsFromString(kDefaultFlags, static_cast<int>(sizeof(kDefaultFlags) - 1));
    v8::V8::InitializeICUDefaultLocation("");
    v8::V8::InitializeExternalStartupData("");
    platform_ = v8::platform::NewDefaultPlatform();
    v8::V8::InitializePlatform(platform_.get());
    v8::V8::Initialize();

    params_.array_buffer_allocator = v8::ArrayBuffer::Allocator::NewDefaultAllocator();
    isolate_ = v8::Isolate::New(params_);
  }

  ~V8Runtime() {
    isolate_->Dispose();
    delete params_.array_buffer_allocator;
    v8::V8::Dispose();
    v8::V8::DisposePlatform();
  }

  v8::Isolate* isolate() const { return isolate_; }

 private:
  std::unique_ptr<v8::Platform> platform_;
  v8::Isolate::CreateParams params_{};
  v8::Isolate* isolate_ = nullptr;
};

class FixtureTestBase : public ::testing::Test {
 protected:
  static void SetUpTestSuite() { runtime_ = std::make_unique<V8Runtime>(); }
  static void TearDownTestSuite() { runtime_.reset(); }
  static std::unique_ptr<V8Runtime> runtime_;
};

inline std::unique_ptr<V8Runtime> FixtureTestBase::runtime_;

struct EnvScope {
  explicit EnvScope(V8Runtime* runtime)
      : isolate(runtime->isolate()),
        isolate_scope(isolate),
        handle_scope(isolate),
        context(v8::Context::New(isolate)),
        context_scope(context) {
    EXPECT_EQ(unofficial_napi_create_env_from_context(context, 8, &env), napi_ok);
    EXPECT_NE(env, nullptr);
  }

  ~EnvScope() {
    if (env != nullptr) {
      EXPECT_EQ(unofficial_napi_destroy_env_instance(env), napi_ok);
      env = nullptr;
    }
  }

  v8::Isolate* isolate;
  v8::Isolate::Scope isolate_scope;
  v8::HandleScope handle_scope;
  v8::Local<v8::Context> context;
  v8::Context::Scope context_scope;
  napi_env env = nullptr;
};

#endif  // NAPI_V8_TEST_ENV_H_
