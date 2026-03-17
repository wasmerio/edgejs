#ifndef NAPI_JSC_TEST_ENV_H_
#define NAPI_JSC_TEST_ENV_H_

#include <memory>

#include <gtest/gtest.h>

#include "unofficial_napi.h"

class JscRuntime {
 public:
  JscRuntime() = default;
  ~JscRuntime() = default;
};

class FixtureTestBase : public ::testing::Test {
 protected:
  static void SetUpTestSuite() { runtime_ = std::make_unique<JscRuntime>(); }
  static void TearDownTestSuite() { runtime_.reset(); }
  static std::unique_ptr<JscRuntime> runtime_;
};

inline std::unique_ptr<JscRuntime> FixtureTestBase::runtime_;

struct EnvScope {
  explicit EnvScope(JscRuntime* /*runtime*/) {
    EXPECT_EQ(unofficial_napi_create_env(8, &env, &scope), napi_ok);
    EXPECT_NE(env, nullptr);
    EXPECT_NE(scope, nullptr);
  }

  ~EnvScope() {
    if (scope != nullptr) {
      EXPECT_EQ(unofficial_napi_release_env(scope), napi_ok);
      scope = nullptr;
      env = nullptr;
    }
  }

  napi_env env = nullptr;
  void* scope = nullptr;
};

#endif  // NAPI_JSC_TEST_ENV_H_
