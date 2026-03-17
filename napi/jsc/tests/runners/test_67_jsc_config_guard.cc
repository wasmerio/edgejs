#include <JavaScriptCore/JavaScript.h>

#include "../../src/internal/napi_jsc_env.h"
#include "test_env.h"

#include "unofficial_napi.h"

class Test67JscConfigGuard : public FixtureTestBase {};

namespace {

bool HostHasSharedArrayBuffer() {
  napi_jsc_prepare_runtime_for_context_creation();
  JSGlobalContextRef context = JSGlobalContextCreate(nullptr);
  if (context == nullptr) return false;
  JSValueRef exception = nullptr;
  JSStringRef source = JSStringCreateWithUTF8CString("typeof SharedArrayBuffer === 'function'");
  JSValueRef result = JSEvaluateScript(context, source, nullptr, nullptr, 0, &exception);
  JSStringRelease(source);
  const bool has_sab = result != nullptr && exception == nullptr && JSValueToBoolean(context, result);
  JSGlobalContextRelease(context);
  return has_sab;
}

}  // namespace

TEST_F(Test67JscConfigGuard, RejectsSablessHostConfiguration) {
  if (HostHasSharedArrayBuffer()) {
    GTEST_SKIP() << "Host JavaScriptCore exposes SharedArrayBuffer; negative config guard not applicable";
  }

  napi_env env = nullptr;
  void* scope = nullptr;
  EXPECT_EQ(unofficial_napi_create_env(8, &env, &scope), napi_generic_failure);
  EXPECT_EQ(env, nullptr);
  EXPECT_EQ(scope, nullptr);
}
