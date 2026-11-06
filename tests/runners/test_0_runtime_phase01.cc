#include <cstdlib>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <thread>
#include <vector>

#include "test_env.h"
#include "edge_path.h"
#include "edge_env_loop.h"
#include "edge_runtime.h"
#include "edge_runtime_platform.h"
#include "edge_task_queue.h"
#include "edge_timers_host.h"

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

std::string GetGlobalUtf8(napi_env env, const char* name) {
  napi_value global = nullptr;
  if (napi_get_global(env, &global) != napi_ok || global == nullptr) return "";

  napi_value value = nullptr;
  if (napi_get_named_property(env, global, name, &value) != napi_ok || value == nullptr) return "";
  return ValueToUtf8(env, value);
}

constexpr const char* kZlibRoundTripScript = R"JS(
require('internal/util/debuglog').initializeDebugEnv(process.env.NODE_DEBUG);
const assert = require('assert');
const zlib = require('zlib');

const input = Buffer.from('edge-zlib-roundtrip');
const syncCompressed = zlib.gzipSync(input);
assert.strictEqual(zlib.gunzipSync(syncCompressed).toString(), 'edge-zlib-roundtrip');

// Force processChunkSync() to continue with progressively smaller ranges of
// the same incompressible input. A host-JS provider copies host buffers into
// guest memory when a lease is acquired, so the native binding must retain one
// input lease for this logical operation instead of recopying the remaining
// input for every output chunk.
const multiChunkInput = Buffer.allocUnsafe(1024 * 1024);
let randomState = 0x12345678;
for (let i = 0; i < multiChunkInput.length; ++i) {
  randomState ^= randomState << 13;
  randomState ^= randomState >>> 17;
  randomState ^= randomState << 5;
  multiChunkInput[i] = randomState;
}
const multiChunkCompressed = zlib.gzipSync(multiChunkInput);
assert.deepStrictEqual(
  zlib.gunzipSync(multiChunkCompressed, { chunkSize: 4 * 1024 }),
  multiChunkInput,
);

zlib.gzip(multiChunkInput, { chunkSize: 1024 }, (err, compressed) => {
  if (err) throw err;
  zlib.gunzip(compressed, { chunkSize: 1024 }, (inflateErr, inflated) => {
    if (inflateErr) throw inflateErr;
    assert.deepStrictEqual(inflated, multiChunkInput);
    globalThis.__edge_zlib_roundtrip = 'edge-zlib-roundtrip';
  });
});
)JS";

struct PlatformImmediateTask {
  std::vector<int>* order = nullptr;
  int value = 0;
  int nested_value = 0;
  int nested_flags = kEdgeRuntimePlatformTaskNone;
};

void RunPlatformImmediateTask(napi_env env, void* data) {
  auto* task = static_cast<PlatformImmediateTask*>(data);
  if (task == nullptr || task->order == nullptr) return;
  task->order->push_back(task->value);
  if (task->nested_value != 0) {
    auto* nested = new PlatformImmediateTask();
    nested->order = task->order;
    nested->value = task->nested_value;
    if (EdgeRuntimePlatformEnqueueTask(env,
                                      RunPlatformImmediateTask,
                                      nested,
                                      [](napi_env, void* p) { delete static_cast<PlatformImmediateTask*>(p); },
                                      task->nested_flags) != napi_ok) {
      delete nested;
    }
  }
}

}  // namespace

TEST_F(Test0RuntimePhase01, ValidFixtureScriptReturnsZero) {
  EnvScope s(runtime_.get());
  const std::string script_path = std::string(NAPI_V8_ROOT_PATH) + "/tests/fixtures/phase0_hello.js";
  ASSERT_TRUE(std::filesystem::exists(script_path)) << "Missing fixture: " << script_path;
  testing::internal::CaptureStdout();

  std::string error;
  const int exit_code = EdgeRunScriptFile(s.env, script_path.c_str(), &error);
  const std::string stdout_output = testing::internal::GetCapturedStdout();
  EXPECT_EQ(exit_code, 0) << "error=" << error << ", fixture=" << script_path;
  EXPECT_TRUE(error.empty()) << "error=" << error;
  EXPECT_NE(stdout_output.find("hello from edge"), std::string::npos);
}

TEST_F(Test0RuntimePhase01, ThrownErrorReturnsNonZero) {
  EnvScope s(runtime_.get());
  const std::string script_path = WriteTempScript("edge_phase01_throw", "throw new Error('boom from edge');");

  std::string error;
  const int exit_code = EdgeRunScriptFile(s.env, script_path.c_str(), &error);
  EXPECT_EQ(exit_code, 1);
  EXPECT_NE(error.find("boom from edge"), std::string::npos);

  RemoveTempScript(script_path);
}

TEST_F(Test0RuntimePhase01, SyntaxErrorReturnsNonZero) {
  EnvScope s(runtime_.get());
  const std::string script_path = WriteTempScript("edge_phase01_syntax", "function (");

  std::string error;
  const int exit_code = EdgeRunScriptFile(s.env, script_path.c_str(), &error);
  EXPECT_EQ(exit_code, 1);
  EXPECT_FALSE(error.empty());

  RemoveTempScript(script_path);
}

TEST_F(Test0RuntimePhase01, EsmImportOfThrowingCjsReturnsJsErrorInsteadOfCrashing) {
  const auto temp_dir = std::filesystem::temp_directory_path() / "edge_phase01_esm_import_cjs_throw";
  std::error_code mkdir_ec;
  std::filesystem::create_directories(temp_dir, mkdir_ec);
  ASSERT_FALSE(mkdir_ec) << mkdir_ec.message();

  const auto cjs_path = temp_dir / "bad.cjs";
  const auto esm_path = temp_dir / "main.mjs";

  {
    std::ofstream out(cjs_path);
    out << "throw new Error('boom from cjs');\n";
  }
  {
    std::ofstream out(esm_path);
    out << "import './bad.cjs';\n";
    out << "globalThis.__should_not_run = true;\n";
  }

  // A failed static module graph is an uncaught main-module exception and may
  // terminate the process. Exercise that boundary in a child so the test can
  // distinguish the expected JavaScript exit from a native crash.
  EXPECT_EXIT(
      {
        EnvScope s(runtime_.get());
        std::string error;
        const int exit_code =
            EdgeRunScriptFileWithLoop(s.env, esm_path.c_str(), &error, true);
        if (exit_code != 1 || error.find("boom from cjs") == std::string::npos) {
          std::_Exit(2);
        }
        std::_Exit(exit_code);
      },
      ::testing::ExitedWithCode(1),
      "boom from cjs");

  std::error_code remove_ec;
  std::filesystem::remove_all(temp_dir, remove_ec);
}

TEST_F(Test0RuntimePhase01, SourcePathCanBeTestedIndependently) {
  EnvScope s(runtime_.get());

  std::string error;
  const int exit_code = EdgeRunScriptSource(s.env, "globalThis.__phase01_source = 'ok';", &error);
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
  const int exit_code = EdgeRunScriptSource(s.env, "", &error);
  EXPECT_EQ(exit_code, 1);
  EXPECT_EQ(error, "Empty script source");
}

TEST_F(Test0RuntimePhase01, AsyncFsPromiseChainReachesQuiescence) {
  EnvScope s(runtime_.get());
  constexpr const char* kSource = R"JS(
const fs = require('fs');
globalThis.__edge_fs_chain_count = 0;
globalThis.__edge_fs_chain_done = false;
(async () => {
  for (let i = 0; i < 64; i++) {
    await new Promise((resolve, reject) => {
      fs.stat('.', (error) => {
        if (error) {
          reject(error);
          return;
        }
        globalThis.__edge_fs_chain_count++;
        resolve();
      });
    });
  }
  globalThis.__edge_fs_chain_done = true;
})();
)JS";

  std::string error;
  const int exit_code = EdgeRunScriptSourceWithLoop(s.env, kSource, &error, true);
  EXPECT_EQ(exit_code, 0) << "error=" << error;
  EXPECT_TRUE(error.empty()) << "error=" << error;
  EXPECT_EQ(GetGlobalUtf8(s.env, "__edge_fs_chain_count"), "64");
  EXPECT_EQ(GetGlobalUtf8(s.env, "__edge_fs_chain_done"), "true");
}

TEST_F(Test0RuntimePhase01, NativePathResolveNormalizesDotSegments) {
#ifdef _WIN32
  EXPECT_EQ(edge_path::PathResolve("C:\\base\\dir", {"..\\pkg", ".\\entry.js"}),
            "C:\\base\\pkg\\entry.js");
#else
  EXPECT_EQ(edge_path::PathResolve("/tmp/base/dir", {"../pkg", "./entry.js"}),
            "/tmp/base/pkg/entry.js");
#endif
}

TEST_F(Test0RuntimePhase01, TimersHostStateIsIsolatedPerEnv) {
  EnvScope first(runtime_.get());
  EnvScope second(runtime_.get());

  napi_value first_binding = EdgeInstallTimersHostBinding(first.env);
  napi_value second_binding = EdgeInstallTimersHostBinding(second.env);
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

  EXPECT_EQ(EdgeGetActiveTimeoutCount(first.env), 2);
  EXPECT_EQ(EdgeGetActiveTimeoutCount(second.env), 5);
  EXPECT_EQ(EdgeGetActiveImmediateRefCount(first.env), 1u);
  EXPECT_EQ(EdgeGetActiveImmediateRefCount(second.env), 3u);
}

TEST_F(Test0RuntimePhase01, TimersHostStateCanBeDestroyedAndRecreatedAcrossEnvs) {
  {
    EnvScope first(runtime_.get());

    napi_value binding = EdgeInstallTimersHostBinding(first.env);
    ASSERT_NE(binding, nullptr);

    napi_value timeout_info = nullptr;
    napi_value immediate_info = nullptr;
    ASSERT_EQ(napi_get_named_property(first.env, binding, "timeoutInfo", &timeout_info), napi_ok);
    ASSERT_EQ(napi_get_named_property(first.env, binding, "immediateInfo", &immediate_info), napi_ok);

    int32_t* timeout_data = GetInt32TypedArrayData(first.env, timeout_info, 1);
    int32_t* immediate_data = GetInt32TypedArrayData(first.env, immediate_info, 3);
    ASSERT_NE(timeout_data, nullptr);
    ASSERT_NE(immediate_data, nullptr);

    timeout_data[0] = 4;
    immediate_data[1] = 2;

    EXPECT_EQ(EdgeGetActiveTimeoutCount(first.env), 4);
    EXPECT_EQ(EdgeGetActiveImmediateRefCount(first.env), 2u);
  }

  {
    EnvScope second(runtime_.get());

    napi_value binding = EdgeInstallTimersHostBinding(second.env);
    ASSERT_NE(binding, nullptr);

    napi_value timeout_info = nullptr;
    napi_value immediate_info = nullptr;
    ASSERT_EQ(napi_get_named_property(second.env, binding, "timeoutInfo", &timeout_info), napi_ok);
    ASSERT_EQ(napi_get_named_property(second.env, binding, "immediateInfo", &immediate_info), napi_ok);

    int32_t* timeout_data = GetInt32TypedArrayData(second.env, timeout_info, 1);
    int32_t* immediate_data = GetInt32TypedArrayData(second.env, immediate_info, 3);
    ASSERT_NE(timeout_data, nullptr);
    ASSERT_NE(immediate_data, nullptr);

    timeout_data[0] = 1;
    immediate_data[1] = 5;

    EXPECT_EQ(EdgeGetActiveTimeoutCount(second.env), 1);
    EXPECT_EQ(EdgeGetActiveImmediateRefCount(second.env), 5u);
  }
}

TEST_F(Test0RuntimePhase01, TaskQueueStateIsIsolatedPerEnv) {
  EnvScope first(runtime_.get());
  EnvScope second(runtime_.get());

  napi_value first_binding = EdgeGetOrCreateTaskQueueBinding(first.env);
  napi_value second_binding = EdgeGetOrCreateTaskQueueBinding(second.env);
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
  EXPECT_TRUE(EdgeGetTaskQueueFlags(first.env, &first_has_tick_scheduled, &first_has_rejection_to_warn));
  EXPECT_TRUE(EdgeGetTaskQueueFlags(second.env, &second_has_tick_scheduled, &second_has_rejection_to_warn));
  EXPECT_TRUE(first_has_tick_scheduled);
  EXPECT_FALSE(first_has_rejection_to_warn);
  EXPECT_FALSE(second_has_tick_scheduled);
  EXPECT_TRUE(second_has_rejection_to_warn);
}

TEST_F(Test0RuntimePhase01, TickCallbackDoesNotDependOnMutableDispatchGlobals) {
  EnvScope s(runtime_.get());

  napi_value binding = EdgeGetOrCreateTaskQueueBinding(s.env);
  ASSERT_NE(binding, nullptr);
  napi_value set_tick_callback = nullptr;
  ASSERT_EQ(napi_get_named_property(
                s.env, binding, "setTickCallback", &set_tick_callback),
            napi_ok);
  ASSERT_NE(set_tick_callback, nullptr);

  napi_value source = nullptr;
  napi_value callback = nullptr;
  ASSERT_EQ(napi_create_string_utf8(
                s.env,
                "(function processTicksAndRejections() { "
                "globalThis.__edge_tick_receiver_ok = (this === globalThis); })",
                NAPI_AUTO_LENGTH,
                &source),
            napi_ok);
  ASSERT_EQ(napi_run_script(s.env, source, &callback), napi_ok);
  ASSERT_NE(callback, nullptr);
  napi_value ignored = nullptr;
  ASSERT_EQ(napi_call_function(
                s.env, binding, set_tick_callback, 1, &callback, &ignored),
            napi_ok);

  ASSERT_EQ(napi_create_string_utf8(
                s.env,
                "globalThis.Reflect = undefined; "
                "Object.defineProperty(globalThis, 'process', { "
                "configurable: true, get() { throw new Error('process read during tick'); } });",
                NAPI_AUTO_LENGTH,
                &source),
            napi_ok);
  ASSERT_EQ(napi_run_script(s.env, source, &ignored), napi_ok);

  EXPECT_EQ(EdgeRunTaskQueueTickCallback(s.env), napi_ok);
  napi_value global = nullptr;
  napi_value receiver_ok = nullptr;
  bool receiver_matches = false;
  ASSERT_EQ(napi_get_global(s.env, &global), napi_ok);
  ASSERT_EQ(napi_get_named_property(
                s.env, global, "__edge_tick_receiver_ok", &receiver_ok),
            napi_ok);
  ASSERT_EQ(napi_get_value_bool(s.env, receiver_ok, &receiver_matches), napi_ok);
  EXPECT_TRUE(receiver_matches);
}

TEST_F(Test0RuntimePhase01, TickCallbackAddsNoSyntheticJavascriptFrame) {
  EnvScope s(runtime_.get());

  napi_value binding = EdgeGetOrCreateTaskQueueBinding(s.env);
  ASSERT_NE(binding, nullptr);
  napi_value set_tick_callback = nullptr;
  ASSERT_EQ(napi_get_named_property(
                s.env, binding, "setTickCallback", &set_tick_callback),
            napi_ok);

  napi_value source = nullptr;
  napi_value callback = nullptr;
  ASSERT_EQ(napi_create_string_utf8(
                s.env,
                "(function processTicksAndRejections() { "
                "throw new Error('tick stack'); })",
                NAPI_AUTO_LENGTH,
                &source),
            napi_ok);
  ASSERT_EQ(napi_run_script(s.env, source, &callback), napi_ok);
  napi_value ignored = nullptr;
  ASSERT_EQ(napi_call_function(
                s.env, binding, set_tick_callback, 1, &callback, &ignored),
            napi_ok);

  EXPECT_EQ(EdgeRunTaskQueueTickCallback(s.env), napi_pending_exception);
  napi_value exception = nullptr;
  ASSERT_EQ(napi_get_and_clear_last_exception(s.env, &exception), napi_ok);
  napi_value stack = nullptr;
  ASSERT_EQ(napi_get_named_property(s.env, exception, "stack", &stack), napi_ok);
  const std::string stack_text = ValueToUtf8(s.env, stack);
  const std::string frame_name = "processTicksAndRejections";
  const size_t first = stack_text.find(frame_name);
  ASSERT_NE(first, std::string::npos) << stack_text;
  EXPECT_EQ(stack_text.find(frame_name, first + frame_name.size()),
            std::string::npos)
      << stack_text;
}

TEST_F(Test0RuntimePhase01, HandledTickExceptionEndsCurrentCallbackBoundary) {
  EnvScope s(runtime_.get());

  napi_value binding = EdgeGetOrCreateTaskQueueBinding(s.env);
  ASSERT_NE(binding, nullptr);
  napi_value set_tick_callback = nullptr;
  napi_value tick_info = nullptr;
  ASSERT_EQ(napi_get_named_property(
                s.env, binding, "setTickCallback", &set_tick_callback),
            napi_ok);
  ASSERT_EQ(napi_get_named_property(s.env, binding, "tickInfo", &tick_info),
            napi_ok);
  int32_t* tick_fields = GetInt32TypedArrayData(s.env, tick_info, 2);
  ASSERT_NE(tick_fields, nullptr);
  tick_fields[0] = 1;

  napi_value global = nullptr;
  ASSERT_EQ(napi_get_global(s.env, &global), napi_ok);
  ASSERT_EQ(napi_set_named_property(
                s.env, global, "__edge_checkpoint_tick_info", tick_info),
            napi_ok);

  napi_value source = nullptr;
  napi_value ignored = nullptr;
  ASSERT_EQ(napi_create_string_utf8(
                s.env,
                "globalThis.__edge_checkpoint_tick_count = 0;"
                "globalThis.process = { _fatalException() { return true; } };",
                NAPI_AUTO_LENGTH,
                &source),
            napi_ok);
  ASSERT_EQ(napi_run_script(s.env, source, &ignored), napi_ok);

  napi_value tick_callback = nullptr;
  ASSERT_EQ(napi_create_string_utf8(
                s.env,
                "(function processTicksAndRejections() {"
                "  const count = ++globalThis.__edge_checkpoint_tick_count;"
                "  if (count < 3) throw new Error('handled tick failure');"
                "  globalThis.__edge_checkpoint_tick_info[0] = 0;"
                "})",
                NAPI_AUTO_LENGTH,
                &source),
            napi_ok);
  ASSERT_EQ(napi_run_script(s.env, source, &tick_callback), napi_ok);
  ASSERT_EQ(napi_call_function(s.env,
                               binding,
                               set_tick_callback,
                               1,
                               &tick_callback,
                               &ignored),
            napi_ok);

  napi_value callback = nullptr;
  ASSERT_EQ(napi_create_string_utf8(
                s.env, "(function callback() {})", NAPI_AUTO_LENGTH, &source),
            napi_ok);
  ASSERT_EQ(napi_run_script(s.env, source, &callback), napi_ok);
  ASSERT_EQ(EdgeMakeCallback(
                s.env, global, callback, 0, nullptr, &ignored),
            napi_ok);

  // Handling the error completes this native->JS turn. The event loop may
  // schedule another checkpoint, but the callback boundary must not spin by
  // immediately re-entering a provider-owned task queue.
  EXPECT_EQ(GetGlobalUtf8(s.env, "__edge_checkpoint_tick_count"), "1");
  tick_fields[0] = 0;
}

TEST_F(Test0RuntimePhase01, NativeImmediateQueueRunsBeforeJsImmediatesAndDrainsNestedTasks) {
  EnvScope s(runtime_.get());

  auto* first = new PlatformImmediateTask();
  std::vector<int> order;
  first->order = &order;
  first->value = 1;
  first->nested_value = 2;
  ASSERT_EQ(EdgeRuntimePlatformEnqueueTask(s.env,
                                          RunPlatformImmediateTask,
                                          first,
                                          [](napi_env, void* p) { delete static_cast<PlatformImmediateTask*>(p); },
                                          kEdgeRuntimePlatformTaskRefed),
            napi_ok);
  EXPECT_TRUE(EdgeRuntimePlatformHasImmediateTasks(s.env));
  EXPECT_TRUE(EdgeRuntimePlatformHasRefedImmediateTasks(s.env));
  EXPECT_EQ(EdgeRuntimePlatformDrainImmediateTasks(s.env), 2u);

  ASSERT_EQ(order.size(), 2u);
  EXPECT_EQ(order[0], 1);
  EXPECT_EQ(order[1], 2);
  EXPECT_FALSE(EdgeRuntimePlatformHasImmediateTasks(s.env));
  EXPECT_FALSE(EdgeRuntimePlatformHasRefedImmediateTasks(s.env));
}

TEST_F(Test0RuntimePhase01, ForegroundHookDefersCrossThreadWorkAndCleansItOnce) {
  struct Task {
    std::vector<int> order;
    std::thread::id owning_thread = std::this_thread::get_id();
    napi_env env = nullptr;
  } task;
  EnvScope s(runtime_.get());
  task.env = s.env;
  unofficial_napi_env_hooks hooks{};
  ASSERT_EQ(EdgeRuntimePlatformPrepareEnvHooks(s.env, &hooks), napi_ok);
  napi_status submitted = napi_generic_failure;
  std::thread producer([&]() {
    submitted = hooks.enqueue_foreground_task_callback(
        hooks.data,
        [](napi_env env, void* data) {
          auto* task = static_cast<Task*>(data);
          EXPECT_EQ(env, task->env);
          EXPECT_EQ(std::this_thread::get_id(), task->owning_thread);
          task->order.push_back(1);
        },
        &task,
        [](napi_env env, void* data) {
          auto* task = static_cast<Task*>(data);
          EXPECT_EQ(env, task->env);
          task->order.push_back(2);
        },
        0);
  });
  producer.join();
  ASSERT_EQ(submitted, napi_ok);
  EXPECT_TRUE(task.order.empty());
  ASSERT_EQ(EdgeRuntimePlatformDrainTasks(s.env), napi_ok);
  EXPECT_EQ(task.order, (std::vector<int>{1, 2}));
  ASSERT_EQ(EdgeRuntimePlatformDrainTasks(s.env), napi_ok);
  EXPECT_EQ(task.order, (std::vector<int>{1, 2}));
}

TEST_F(Test0RuntimePhase01, DelayedForegroundHookCleansBeforeTheNextTimerRuns) {
  std::vector<int> order;
  EnvScope s(runtime_.get());
  unofficial_napi_env_hooks hooks{};
  ASSERT_EQ(EdgeRuntimePlatformPrepareEnvHooks(s.env, &hooks), napi_ok);
  ASSERT_EQ(EdgeRuntimePlatformAddRef(s.env), napi_ok);
  ASSERT_EQ(hooks.enqueue_foreground_task_callback(
                hooks.data,
                [](napi_env, void* data) { static_cast<std::vector<int>*>(data)->push_back(1); },
                &order,
                [](napi_env, void* data) { static_cast<std::vector<int>*>(data)->push_back(2); },
                1),
            napi_ok);
  ASSERT_EQ(hooks.enqueue_foreground_task_callback(
                hooks.data,
                [](napi_env, void* data) {
                  auto* order = static_cast<std::vector<int>*>(data);
                  EXPECT_EQ(*order, (std::vector<int>{1, 2}));
                  order->push_back(3);
                },
                &order,
                [](napi_env, void* data) { static_cast<std::vector<int>*>(data)->push_back(4); },
                1),
            napi_ok);
  EXPECT_TRUE(order.empty());
  std::this_thread::sleep_for(std::chrono::milliseconds(5));
  (void)uv_run(EdgeGetEnvLoop(s.env), UV_RUN_NOWAIT);
  EXPECT_EQ(order, (std::vector<int>{1, 2, 3, 4}));
  ASSERT_EQ(EdgeRuntimePlatformReleaseRef(s.env), napi_ok);
}

TEST_F(Test0RuntimePhase01, ForegroundHookCleansDroppedAndRefusedTasksWithTheirEnv) {
  struct Task {
    napi_env env = nullptr;
    size_t cleaned = 0;
  } task;
  EnvScope s(runtime_.get());
  task.env = s.env;
  unofficial_napi_env_hooks hooks{};
  ASSERT_EQ(EdgeRuntimePlatformPrepareEnvHooks(s.env, &hooks), napi_ok);
  const auto callback = [](napi_env, void*) { ADD_FAILURE() << "dropped task ran"; };
  const auto cleanup = [](napi_env env, void* data) {
    auto* task = static_cast<Task*>(data);
    EXPECT_EQ(env, task->env);
    ++task->cleaned;
  };
  ASSERT_EQ(hooks.enqueue_foreground_task_callback(hooks.data, callback, &task, cleanup, 0),
            napi_ok);
  ASSERT_EQ(hooks.enqueue_foreground_task_callback(hooks.data, callback, &task, cleanup, 60000),
            napi_ok);
  EdgeRunRuntimePlatformEnvCleanup(s.env);
  EXPECT_EQ(task.cleaned, 2u);
  EXPECT_EQ(hooks.enqueue_foreground_task_callback(hooks.data, callback, &task, cleanup, 0),
            napi_generic_failure);
  EXPECT_EQ(task.cleaned, 3u);
  EXPECT_EQ(hooks.enqueue_foreground_task_callback(hooks.data, nullptr, &task, cleanup, 0),
            napi_invalid_arg);
  EXPECT_EQ(task.cleaned, 4u);
  (void)uv_run(EdgeGetEnvLoop(s.env), UV_RUN_NOWAIT);
  EXPECT_EQ(task.cleaned, 4u);
}

TEST_F(Test0RuntimePhase01, ZlibWriteResultStateCanBeDestroyedAndRecreatedAcrossEnvs) {
  {
    EnvScope first(runtime_.get());

    std::string error;
    const int exit_code = EdgeRunScriptSourceWithLoop(first.env, kZlibRoundTripScript, &error, true);
    EXPECT_EQ(exit_code, 0) << "error=" << error;
    EXPECT_TRUE(error.empty());
    EXPECT_EQ(GetGlobalUtf8(first.env, "__edge_zlib_roundtrip"), "edge-zlib-roundtrip");
  }

  {
    EnvScope second(runtime_.get());

    std::string error;
    const int exit_code = EdgeRunScriptSourceWithLoop(second.env, kZlibRoundTripScript, &error, true);
    EXPECT_EQ(exit_code, 0) << "error=" << error;
    EXPECT_TRUE(error.empty());
    EXPECT_EQ(GetGlobalUtf8(second.env, "__edge_zlib_roundtrip"), "edge-zlib-roundtrip");
  }
}
