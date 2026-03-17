#include <algorithm>
#include <string>

#include "test_env.h"
#include "upstream_js_test.h"
#include "unofficial_napi.h"

extern "C" napi_value Init(napi_env env, napi_value exports);

class Test14Exception : public FixtureTestBase {};

namespace {

std::string ValueToUtf8(napi_env env, napi_value value) {
  if (env == nullptr || value == nullptr) return {};
  size_t length = 0;
  if (napi_get_value_string_utf8(env, value, nullptr, 0, &length) != napi_ok) return {};
  std::string out(length + 1, '\0');
  size_t copied = 0;
  if (napi_get_value_string_utf8(env, value, out.data(), out.size(), &copied) != napi_ok) return {};
  out.resize(copied);
  return out;
}

std::string GetErrorMessage(napi_env env, napi_value exception) {
  if (env == nullptr || exception == nullptr) return {};
  napi_value message = nullptr;
  if (napi_get_named_property(env, exception, "message", &message) != napi_ok) return {};
  return ValueToUtf8(env, message);
}

}  // namespace

TEST_F(Test14Exception, PortedCoreFlow) {
  EnvScope s(runtime_.get());
  napi_value exports = nullptr;
  ASSERT_EQ(napi_create_object(s.env, &exports), napi_ok);
  (void)Init(s.env, exports);

  bool pending = false;
  ASSERT_EQ(napi_is_exception_pending(s.env, &pending), napi_ok);
  ASSERT_TRUE(pending);
  napi_value init_error = nullptr;
  ASSERT_EQ(napi_get_and_clear_last_exception(s.env, &init_error), napi_ok);

  napi_value binding = nullptr;
  ASSERT_EQ(napi_get_named_property(s.env, init_error, "binding", &binding), napi_ok);
  ASSERT_TRUE(InstallUpstreamJsShim(s, binding));
  ASSERT_TRUE(SetUpstreamRequireException(s, init_error));
  ASSERT_TRUE(RunUpstreamJsFile(
      s, std::string(NAPI_TESTS_ROOT_PATH) + "/js-native-api/test_exception/test.js"));
}

TEST_F(Test14Exception, SetLastExceptionStoresThrownErrorMessage) {
  EnvScope s(runtime_.get());

  napi_value script = nullptr;
  ASSERT_EQ(
      napi_create_string_utf8(
          s.env, "throw new Error('boom')", NAPI_AUTO_LENGTH, &script),
      napi_ok);
  napi_value result = nullptr;
  ASSERT_EQ(napi_run_script(s.env, script, &result), napi_pending_exception);

  napi_value exception = nullptr;
  ASSERT_EQ(napi_get_and_clear_last_exception(s.env, &exception), napi_ok);
  ASSERT_NE(exception, nullptr);
  EXPECT_EQ(GetErrorMessage(s.env, exception), "boom");
}

TEST_F(Test14Exception, SetLastExceptionPreservesThrownErrorMessageAcrossSameErrorRethrow) {
  EnvScope s(runtime_.get());

  napi_value script = nullptr;
  ASSERT_EQ(
      napi_create_string_utf8(
          s.env, "throw new Error('boom')", NAPI_AUTO_LENGTH, &script),
      napi_ok);
  napi_value result = nullptr;
  ASSERT_EQ(napi_run_script(s.env, script, &result), napi_pending_exception);

  napi_value first = nullptr;
  ASSERT_EQ(napi_get_and_clear_last_exception(s.env, &first), napi_ok);
  ASSERT_NE(first, nullptr);
  const std::string first_message = GetErrorMessage(s.env, first);
  ASSERT_EQ(first_message, "boom");

  ASSERT_EQ(napi_throw(s.env, first), napi_pending_exception);

  napi_value second = nullptr;
  ASSERT_EQ(napi_get_and_clear_last_exception(s.env, &second), napi_ok);
  ASSERT_NE(second, nullptr);
  EXPECT_EQ(GetErrorMessage(s.env, second), first_message);
}
