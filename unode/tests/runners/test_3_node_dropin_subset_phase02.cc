#include <string>

#include "test_env.h"
#include "unode_runtime.h"

class Test3NodeDropinSubsetPhase02 : public FixtureTestBase {};

namespace {

int RunNodeCompatScript(napi_env env, const char* relative_path, std::string* error_out) {
  const std::string script_path = std::string(NAPI_V8_ROOT_PATH) + "/tests/node-compat/" + relative_path;
  return UnodeRunScriptFile(env, script_path.c_str(), error_out);
}

}  // namespace

TEST_F(Test3NodeDropinSubsetPhase02, RequireCacheSubsetTest) {
  EnvScope s(runtime_.get());
  std::string error;
  const int exit_code = RunNodeCompatScript(s.env, "parallel/test-require-cache.js", &error);
  EXPECT_EQ(exit_code, 0) << "error=" << error;
  EXPECT_TRUE(error.empty()) << "error=" << error;
}

TEST_F(Test3NodeDropinSubsetPhase02, RequireJsonSubsetTest) {
  EnvScope s(runtime_.get());
  std::string error;
  const int exit_code = RunNodeCompatScript(s.env, "parallel/test-require-json.js", &error);
  EXPECT_EQ(exit_code, 0) << "error=" << error;
  EXPECT_TRUE(error.empty()) << "error=" << error;
}
