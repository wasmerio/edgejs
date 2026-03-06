#include <filesystem>
#include <fstream>
#include <functional>
#include <string>

#include "test_env.h"
#include "ubi_path.h"
#include "ubi_runtime.h"
#include "ubi_task_queue.h"
#include "ubi_timers_host.h"

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

int32_t* GetInt32TypedArrayData(napi_env env, napi_value value, size_t expected_length) {
  napi_typedarray_type type = napi_int8_array;
  size_t length = 0;
  void* data = nullptr;
  napi_value arraybuffer = nullptr;
  size_t byte_offset = 0;
  if (napi_get_typedarray_info(env, value, &type, &length, &data, &arraybuffer, &byte_offset) != napi_ok) {
    return nullptr;
  }
  if (type != napi_int32_array || length < expected_length || data == nullptr) {
    return nullptr;
  }
  return static_cast<int32_t*>(data);
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

TEST_F(Test0RuntimePhase01, TimersHostStateIsIsolatedPerEnv) {
  EnvScope first(runtime_.get());
  EnvScope second(runtime_.get());

  napi_value first_binding = UbiInstallTimersHostBinding(first.env);
  napi_value second_binding = UbiInstallTimersHostBinding(second.env);
  ASSERT_NE(first_binding, nullptr);
  ASSERT_NE(second_binding, nullptr);

  napi_value first_timeout_info = nullptr;
  napi_value second_timeout_info = nullptr;
  napi_value first_immediate_info = nullptr;
  napi_value second_immediate_info = nullptr;
  ASSERT_EQ(napi_get_named_property(first.env, first_binding, "timeoutInfo", &first_timeout_info), napi_ok);
  ASSERT_EQ(napi_get_named_property(second.env, second_binding, "timeoutInfo", &second_timeout_info), napi_ok);
  ASSERT_EQ(napi_get_named_property(first.env, first_binding, "immediateInfo", &first_immediate_info), napi_ok);
  ASSERT_EQ(napi_get_named_property(second.env, second_binding, "immediateInfo", &second_immediate_info), napi_ok);

  int32_t* first_timeout_data = GetInt32TypedArrayData(first.env, first_timeout_info, 1);
  int32_t* second_timeout_data = GetInt32TypedArrayData(second.env, second_timeout_info, 1);
  int32_t* first_immediate_data = GetInt32TypedArrayData(first.env, first_immediate_info, 3);
  int32_t* second_immediate_data = GetInt32TypedArrayData(second.env, second_immediate_info, 3);
  ASSERT_NE(first_timeout_data, nullptr);
  ASSERT_NE(second_timeout_data, nullptr);
  ASSERT_NE(first_immediate_data, nullptr);
  ASSERT_NE(second_immediate_data, nullptr);

  first_timeout_data[0] = 2;
  second_timeout_data[0] = 5;
  first_immediate_data[1] = 1;
  second_immediate_data[1] = 3;

  EXPECT_EQ(UbiGetActiveTimeoutCount(first.env), 2);
  EXPECT_EQ(UbiGetActiveTimeoutCount(second.env), 5);
  EXPECT_EQ(UbiGetActiveImmediateRefCount(first.env), 1u);
  EXPECT_EQ(UbiGetActiveImmediateRefCount(second.env), 3u);
}

TEST_F(Test0RuntimePhase01, TaskQueueStateIsIsolatedPerEnv) {
  EnvScope first(runtime_.get());
  EnvScope second(runtime_.get());

  napi_value first_binding = UbiGetOrCreateTaskQueueBinding(first.env);
  napi_value second_binding = UbiGetOrCreateTaskQueueBinding(second.env);
  ASSERT_NE(first_binding, nullptr);
  ASSERT_NE(second_binding, nullptr);

  napi_value first_tick_info = nullptr;
  napi_value second_tick_info = nullptr;
  ASSERT_EQ(napi_get_named_property(first.env, first_binding, "tickInfo", &first_tick_info), napi_ok);
  ASSERT_EQ(napi_get_named_property(second.env, second_binding, "tickInfo", &second_tick_info), napi_ok);

  int32_t* first_tick_fields = GetInt32TypedArrayData(first.env, first_tick_info, 2);
  int32_t* second_tick_fields = GetInt32TypedArrayData(second.env, second_tick_info, 2);
  ASSERT_NE(first_tick_fields, nullptr);
  ASSERT_NE(second_tick_fields, nullptr);

  first_tick_fields[0] = 1;
  first_tick_fields[1] = 0;
  second_tick_fields[0] = 0;
  second_tick_fields[1] = 1;

  bool first_has_tick_scheduled = false;
  bool first_has_rejection_to_warn = false;
  bool second_has_tick_scheduled = false;
  bool second_has_rejection_to_warn = false;
  EXPECT_TRUE(UbiGetTaskQueueFlags(first.env, &first_has_tick_scheduled, &first_has_rejection_to_warn));
  EXPECT_TRUE(UbiGetTaskQueueFlags(second.env, &second_has_tick_scheduled, &second_has_rejection_to_warn));
  EXPECT_TRUE(first_has_tick_scheduled);
  EXPECT_FALSE(first_has_rejection_to_warn);
  EXPECT_FALSE(second_has_tick_scheduled);
  EXPECT_TRUE(second_has_rejection_to_warn);
}
