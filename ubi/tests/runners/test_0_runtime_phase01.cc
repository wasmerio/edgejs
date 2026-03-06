#include <filesystem>
#include <fstream>
#include <functional>
#include <string>

#include "test_env.h"
#include "ubi_path.h"
#include "ubi_runtime.h"

class Test0RuntimePhase01 : public FixtureTestBase {};

namespace {

std::string ValueToUtf8(napi_env env, napi_value value) {
  napi_value string_value = nullptr;
  if (napi_coerce_to_string(env, value, &string_value) != napi_ok || string_value == nullptr) {
    return "";
  }
  size_t length = 0;
  if (napi_get_value_string_utf8(env, string_value, nullptr, 0, &length) != napi_ok) {
    return "";
  }
  std::string out(length + 1, '\0');
  size_t copied = 0;
  if (napi_get_value_string_utf8(env, string_value, out.data(), out.size(), &copied) != napi_ok) {
    return "";
  }
  out.resize(copied);
  return out;
}

std::string WriteTempScript(const std::string& stem, const std::string& contents) {
  const auto temp_dir = std::filesystem::temp_directory_path();
  const auto unique_name =
      stem + "_" + std::to_string(static_cast<unsigned long long>(std::hash<std::string>{}(contents))) + ".js";
  const auto script_path = temp_dir / unique_name;
  std::ofstream out(script_path);
  out << contents;
  out.close();
  return script_path.string();
}

void RemoveTempScript(const std::string& path) {
  std::error_code ec;
  std::filesystem::remove(path, ec);
}

}  // namespace

TEST_F(Test0RuntimePhase01, ValidFixtureScriptReturnsZero) {
  EnvScope s(runtime_.get());
  const std::string script_path = std::string(NAPI_V8_ROOT_PATH) + "/tests/fixtures/phase0_hello.js";
  ASSERT_TRUE(std::filesystem::exists(script_path)) << "Missing fixture: " << script_path;
  testing::internal::CaptureStdout();

  std::string error;
  const int exit_code = UbiRunScriptFile(s.env, script_path.c_str(), &error);
  const std::string stdout_output = testing::internal::GetCapturedStdout();
  EXPECT_EQ(exit_code, 0) << "error=" << error << ", fixture=" << script_path;
  EXPECT_TRUE(error.empty()) << "error=" << error;
  EXPECT_NE(stdout_output.find("hello from ubi"), std::string::npos);
}

TEST_F(Test0RuntimePhase01, ThrownErrorReturnsNonZero) {
  EnvScope s(runtime_.get());
  const std::string script_path = WriteTempScript("ubi_phase01_throw", "throw new Error('boom from ubi');");

  std::string error;
  const int exit_code = UbiRunScriptFile(s.env, script_path.c_str(), &error);
  EXPECT_EQ(exit_code, 1);
  EXPECT_NE(error.find("boom from ubi"), std::string::npos);

  RemoveTempScript(script_path);
}

TEST_F(Test0RuntimePhase01, SyntaxErrorReturnsNonZero) {
  EnvScope s(runtime_.get());
  const std::string script_path = WriteTempScript("ubi_phase01_syntax", "function (");

  std::string error;
  const int exit_code = UbiRunScriptFile(s.env, script_path.c_str(), &error);
  EXPECT_EQ(exit_code, 1);
  EXPECT_FALSE(error.empty());

  RemoveTempScript(script_path);
}

TEST_F(Test0RuntimePhase01, SourcePathCanBeTestedIndependently) {
  EnvScope s(runtime_.get());

  std::string error;
  const int exit_code = UbiRunScriptSource(s.env, "globalThis.__phase01_source = 'ok';", &error);
  EXPECT_EQ(exit_code, 0);
  EXPECT_TRUE(error.empty());

  napi_value global = nullptr;
  ASSERT_EQ(napi_get_global(s.env, &global), napi_ok);
  napi_value value = nullptr;
  ASSERT_EQ(napi_get_named_property(s.env, global, "__phase01_source", &value), napi_ok);
  EXPECT_EQ(ValueToUtf8(s.env, value), "ok");
}

TEST_F(Test0RuntimePhase01, EmptySourceReturnsNonZero) {
  EnvScope s(runtime_.get());

  std::string error;
  const int exit_code = UbiRunScriptSource(s.env, "", &error);
  EXPECT_EQ(exit_code, 1);
  EXPECT_EQ(error, "Empty script source");
}

TEST_F(Test0RuntimePhase01, NativePathResolveNormalizesDotSegments) {
#ifdef _WIN32
  EXPECT_EQ(ubi_path::PathResolve("C:\\base\\dir", {"..\\pkg", ".\\entry.js"}),
            "C:\\base\\pkg\\entry.js");
#else
  EXPECT_EQ(ubi_path::PathResolve("/tmp/base/dir", {"../pkg", "./entry.js"}),
            "/tmp/base/pkg/entry.js");
#endif
}
