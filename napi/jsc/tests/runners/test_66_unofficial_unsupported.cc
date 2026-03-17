#include "test_env.h"

#include <string_view>

#include "unofficial_napi.h"

class Test66UnofficialUnsupported : public FixtureTestBase {};

namespace {

void ExpectUnsupported(napi_env env,
                       napi_status actual,
                       std::string_view api_name) {
  ASSERT_EQ(actual, napi_generic_failure);
  const napi_extended_error_info* info = nullptr;
  ASSERT_EQ(napi_get_last_error_info(env, &info), napi_ok);
  ASSERT_NE(info, nullptr);
  ASSERT_NE(info->error_message, nullptr);
  const std::string_view message(info->error_message);
  EXPECT_NE(message.find(api_name), std::string_view::npos);
  EXPECT_NE(message.find("unsupported on JavaScriptCore"), std::string_view::npos);
}

}  // namespace

TEST_F(Test66UnofficialUnsupported, ReturnsUniformUnsupportedError) {
  EnvScope s(runtime_.get());

  napi_value object = nullptr;
  ASSERT_EQ(napi_create_object(s.env, &object), napi_ok);

  bool bool_out = true;
  ExpectUnsupported(s.env,
                    unofficial_napi_get_proxy_details(s.env, object, nullptr, nullptr),
                    "unofficial_napi_get_proxy_details");
  ExpectUnsupported(s.env,
                    unofficial_napi_contextify_start_sigint_watchdog(s.env, &bool_out),
                    "unofficial_napi_contextify_start_sigint_watchdog");
  ExpectUnsupported(s.env,
                    unofficial_napi_module_wrap_get_status(s.env, nullptr, nullptr),
                    "unofficial_napi_module_wrap_get_status");
}
