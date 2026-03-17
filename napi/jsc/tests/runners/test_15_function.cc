#include "test_env.h"
#include "upstream_js_test.h"
#include "../../src/internal/napi_jsc_env.h"

extern "C" napi_value Init(napi_env env, napi_value exports);

class Test15Function : public FixtureTestBase {};

TEST_F(Test15Function, PortedCoreFlow) {
  EnvScope s(runtime_.get());
  napi_value exports = nullptr;
  ASSERT_EQ(napi_create_object(s.env, &exports), napi_ok);
  napi_value addon = Init(s.env, exports);
  ASSERT_NE(addon, nullptr);
  ASSERT_TRUE(InstallUpstreamJsShim(s, addon));
  const std::string path =
      std::string(NAPI_TESTS_ROOT_PATH) + "/js-native-api/test_function/test.js";
  ASSERT_TRUE(RunUpstreamJsFileNoMustCallVerification(s, path));
  ASSERT_EQ(napi_jsc_sweep_wrapper_finalizers(s.env, true), napi_ok);
  ASSERT_TRUE(RunScript(s, "__napi_verify_must_call();", "must-call-verification"));
}
