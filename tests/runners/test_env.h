#ifndef NAPI_V8_TEST_ENV_H_
#define NAPI_V8_TEST_ENV_H_

#include <memory>

#include <gtest/gtest.h>

#include "edge_environment_runtime.h"
#include "edge_runtime_platform.h"
#include "edge_timers_host.h"
#include "unofficial_napi.h"

class V8Runtime {
 public:
  V8Runtime() {
    unofficial_napi_runtime_options options{};
    options.size = sizeof(options);
    options.version = UNOFFICIAL_NAPI_RUNTIME_OPTIONS_VERSION;
    EXPECT_EQ(unofficial_napi_configure_runtime(&options), napi_ok);
  }
  ~V8Runtime() = default;
};

class FixtureTestBase : public ::testing::Test {
 protected:
  static void SetUpTestSuite() { runtime_ = std::make_unique<V8Runtime>(); }
  static void TearDownTestSuite() { runtime_.reset(); }
  static std::unique_ptr<V8Runtime> runtime_;
};

inline std::unique_ptr<V8Runtime> FixtureTestBase::runtime_;

struct EnvScope {
  struct IsolateShim {
    explicit IsolateShim(napi_env env_in) : env(env_in) {}
    napi_env env = nullptr;
  };

  explicit EnvScope(V8Runtime* runtime) {
    (void)runtime;
    EXPECT_EQ(unofficial_napi_create_env(8, nullptr, &env, &owner), napi_ok);
    EXPECT_NE(env, nullptr);
    if (env != nullptr) {
      EXPECT_TRUE(EdgeAttachEnvironmentForRuntime(env));
      EXPECT_EQ(EdgeInitializeTimersHost(env), napi_ok);
    }
    isolate = std::make_unique<IsolateShim>(env);
  }

  ~EnvScope() {
    isolate.reset();
    if (env != nullptr) {
      // Edge owns its cleanup lifecycle. Drain and detach Edge resources
      // before asking the provider to destroy the JavaScript environment.
      EdgeEnvironmentRunCleanup(env);
      EdgeEnvironmentRunAtExitCallbacks(env);
      EdgeEnvironmentDetach(env);
      EXPECT_EQ(unofficial_napi_release_env(owner, nullptr), napi_ok);
      env = nullptr;
      owner = nullptr;
    }
  }

  std::unique_ptr<IsolateShim> isolate;
  unofficial_napi_env_owner owner = nullptr;
  napi_env env = nullptr;
};

#endif  // NAPI_V8_TEST_ENV_H_
