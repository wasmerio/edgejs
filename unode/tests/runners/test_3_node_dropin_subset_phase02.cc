#include <cstdlib>
#include <filesystem>
#include <string>

#include "test_env.h"
#include "unode_module_loader.h"
#include "unode_runtime.h"

class Test3NodeDropinSubsetPhase02 : public FixtureTestBase {};

namespace {

int RunNodeCompatScript(napi_env env, const char* relative_path, std::string* error_out) {
  namespace fs = std::filesystem;
  const std::string unode_root(NAPI_V8_ROOT_PATH);
  fs::path unode_root_path(unode_root);
  if (!unode_root_path.is_absolute()) {
    fs::path search = fs::current_path();
    const fs::path builtins_relative = unode_root_path / "tests" / "node-compat" / "builtins";
    bool found = false;
    for (; !search.empty() && search != search.parent_path(); search = search.parent_path()) {
      fs::path candidate = (search / builtins_relative).lexically_normal();
      if (fs::exists(candidate)) {
        unode_root_path = fs::absolute(search / unode_root_path);
        found = true;
        break;
      }
    }
    if (!found) {
      unode_root_path = fs::absolute(fs::current_path().parent_path() / unode_root_path);
    }
  } else {
    unode_root_path = fs::absolute(unode_root_path);
  }
  const std::string fallback_builtins =
      (unode_root_path / "tests" / "node-compat" / "builtins").string();
  const std::string script_path =
      (unode_root_path / "tests" / "node-compat" / relative_path).string();
  setenv("UNODE_FALLBACK_BUILTINS_DIR", fallback_builtins.c_str(), 1);
  UnodeSetFallbackBuiltinsDir(fallback_builtins.c_str());
  const int exit_code = UnodeRunScriptFile(env, script_path.c_str(), error_out);
  UnodeSetFallbackBuiltinsDir(nullptr);
  unsetenv("UNODE_FALLBACK_BUILTINS_DIR");
  return exit_code;
}

// Run a Node test script from the node repo (raw drop-in). Uses UNODE_FALLBACK_BUILTINS_DIR
// so require('assert'), require('path'), etc. resolve to unode/tests/node-compat/builtins.
// NODE_TEST_DIR points to node/test so common/fixtures.js can resolve fixtures under node/test/fixtures.
int RunRawNodeTestScript(napi_env env, const char* node_test_relative_path, std::string* error_out) {
#ifdef NAPI_V8_NODE_ROOT_PATH
  namespace fs = std::filesystem;
  const std::string node_root(NAPI_V8_NODE_ROOT_PATH);
  const std::string unode_root(NAPI_V8_ROOT_PATH);
  fs::path node_root_path(node_root);
  if (!node_root_path.is_absolute()) {
    // Resolve relative node_root (e.g. "node") by walking up from cwd until we find
    // node_root/test/parallel/<script> so __filename exists (works from build/ or build/tests/).
    fs::path search = fs::current_path();
    bool found = false;
    for (; !search.empty() && search != search.parent_path(); search = search.parent_path()) {
      fs::path candidate = (search / node_root_path / "test" / "parallel" / node_test_relative_path).lexically_normal();
      if (fs::exists(candidate)) {
        node_root_path = (search / node_root_path).lexically_normal();
        found = true;
        break;
      }
    }
    if (!found) {
      node_root_path = fs::absolute(fs::current_path().parent_path() / node_root_path).lexically_normal();
    }
  } else {
    node_root_path = node_root_path.lexically_normal();
  }
  const fs::path script_path = node_root_path / "test" / "parallel" / node_test_relative_path;
  const std::string script_path_absolute = fs::absolute(script_path).string();
  // Resolve unode root so fallback_builtins exists (works from build/ or build/tests/).
  fs::path unode_root_path(unode_root);
  if (!unode_root_path.is_absolute()) {
    fs::path search = fs::current_path();
    const fs::path builtins_relative = unode_root_path / "tests" / "node-compat" / "builtins";
    bool found = false;
    for (; !search.empty() && search != search.parent_path(); search = search.parent_path()) {
      fs::path candidate = (search / builtins_relative).lexically_normal();
      if (fs::exists(candidate)) {
        unode_root_path = fs::absolute(search / unode_root_path);
        found = true;
        break;
      }
    }
    if (!found) {
      unode_root_path = fs::absolute(fs::current_path().parent_path() / unode_root_path);
    }
  } else {
    unode_root_path = fs::absolute(unode_root_path);
  }
  const std::string fallback_builtins =
      (unode_root_path / "tests" / "node-compat" / "builtins").string();
  const std::string node_test_dir = (node_root_path / "test").string();
  setenv("UNODE_FALLBACK_BUILTINS_DIR", fallback_builtins.c_str(), 1);
  setenv("NODE_TEST_DIR", node_test_dir.c_str(), 1);
  UnodeSetFallbackBuiltinsDir(fallback_builtins.c_str());
  const int exit_code = UnodeRunScriptFile(env, script_path_absolute.c_str(), error_out);
  UnodeSetFallbackBuiltinsDir(nullptr);
  unsetenv("UNODE_FALLBACK_BUILTINS_DIR");
  unsetenv("NODE_TEST_DIR");
  return exit_code;
#else
  (void)env;
  (void)node_test_relative_path;
  (void)error_out;
  return -1;
#endif
}

}  // namespace

TEST_F(Test3NodeDropinSubsetPhase02, NodeAssertSubsetTest) {
  EnvScope s(runtime_.get());
  std::string error;
  const int exit_code = RunNodeCompatScript(s.env, "parallel/test-node-assert.js", &error);
  EXPECT_EQ(exit_code, 0) << "error=" << error;
  EXPECT_TRUE(error.empty()) << "error=" << error;
}

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

TEST_F(Test3NodeDropinSubsetPhase02, ModuleLoadingSubsetTest) {
  EnvScope s(runtime_.get());
  std::string error;
  const int exit_code = RunNodeCompatScript(s.env, "parallel/test-module-loading-subset.js", &error);
  EXPECT_EQ(exit_code, 0) << "error=" << error;
  EXPECT_TRUE(error.empty()) << "error=" << error;
}

TEST_F(Test3NodeDropinSubsetPhase02, FsPhaseCSubsetTest) {
  EnvScope s(runtime_.get());
  std::string error;
  const int exit_code = RunNodeCompatScript(s.env, "parallel/test-fs-phase-c-subset.js", &error);
  EXPECT_EQ(exit_code, 0) << "error=" << error;
  EXPECT_TRUE(error.empty()) << "error=" << error;
}

TEST_F(Test3NodeDropinSubsetPhase02, ConsoleCompatTest) {
  EnvScope s(runtime_.get());
  std::string error;
  const int exit_code = RunNodeCompatScript(s.env, "parallel/test-console.js", &error);
  EXPECT_EQ(exit_code, 0) << "error=" << error;
  EXPECT_TRUE(error.empty()) << "error=" << error;
}

TEST_F(Test3NodeDropinSubsetPhase02, ConsoleAssignUndefinedCompatTest) {
  EnvScope s(runtime_.get());
  std::string error;
  const int exit_code = RunNodeCompatScript(s.env, "parallel/test-console-assign-undefined.js", &error);
  EXPECT_EQ(exit_code, 0) << "error=" << error;
  EXPECT_TRUE(error.empty()) << "error=" << error;
}

TEST_F(Test3NodeDropinSubsetPhase02, ConsoleClearCompatTest) {
  EnvScope s(runtime_.get());
  std::string error;
  const int exit_code = RunNodeCompatScript(s.env, "parallel/test-console-clear.js", &error);
  EXPECT_EQ(exit_code, 0) << "error=" << error;
  EXPECT_TRUE(error.empty()) << "error=" << error;
}

TEST_F(Test3NodeDropinSubsetPhase02, ConsoleGroupCompatTest) {
  EnvScope s(runtime_.get());
  std::string error;
  const int exit_code = RunNodeCompatScript(s.env, "parallel/test-console-group.js", &error);
  EXPECT_EQ(exit_code, 0) << "error=" << error;
  EXPECT_TRUE(error.empty()) << "error=" << error;
}

TEST_F(Test3NodeDropinSubsetPhase02, ConsoleMethodsCompatTest) {
  EnvScope s(runtime_.get());
  std::string error;
  const int exit_code = RunNodeCompatScript(s.env, "parallel/test-console-methods.js", &error);
  EXPECT_EQ(exit_code, 0) << "error=" << error;
  EXPECT_TRUE(error.empty()) << "error=" << error;
}

TEST_F(Test3NodeDropinSubsetPhase02, ConsoleInstanceCompatTest) {
  EnvScope s(runtime_.get());
  std::string error;
  const int exit_code = RunNodeCompatScript(s.env, "parallel/test-console-instance.js", &error);
  EXPECT_EQ(exit_code, 0) << "error=" << error;
  EXPECT_TRUE(error.empty()) << "error=" << error;
}

TEST_F(Test3NodeDropinSubsetPhase02, ConsoleTableCompatTest) {
  EnvScope s(runtime_.get());
  std::string error;
  const int exit_code = RunNodeCompatScript(s.env, "parallel/test-console-table.js", &error);
  EXPECT_EQ(exit_code, 0) << "error=" << error;
  EXPECT_TRUE(error.empty()) << "error=" << error;
}

TEST_F(Test3NodeDropinSubsetPhase02, BufferBase64HardeningCompatTest) {
  EnvScope s(runtime_.get());
  std::string error;
  const int exit_code = RunNodeCompatScript(s.env, "parallel/test-buffer-base64-hardening.js", &error);
  EXPECT_EQ(exit_code, 0) << "error=" << error;
  EXPECT_TRUE(error.empty()) << "error=" << error;
}

TEST_F(Test3NodeDropinSubsetPhase02, OsCompatTest) {
  EnvScope s(runtime_.get());
  std::string error;
  const int exit_code = RunNodeCompatScript(s.env, "parallel/test-os.js", &error);
  EXPECT_EQ(exit_code, 0) << "error=" << error;
  EXPECT_TRUE(error.empty()) << "error=" << error;
}

TEST_F(Test3NodeDropinSubsetPhase02, OsEolCompatTest) {
  EnvScope s(runtime_.get());
  std::string error;
  const int exit_code = RunNodeCompatScript(s.env, "parallel/test-os-eol.js", &error);
  EXPECT_EQ(exit_code, 0) << "error=" << error;
  EXPECT_TRUE(error.empty()) << "error=" << error;
}

TEST_F(Test3NodeDropinSubsetPhase02, OsPriorityCompatTest) {
  EnvScope s(runtime_.get());
  std::string error;
  const int exit_code = RunNodeCompatScript(s.env, "parallel/test-os-process-priority.js", &error);
  EXPECT_EQ(exit_code, 0) << "error=" << error;
  EXPECT_TRUE(error.empty()) << "error=" << error;
}

TEST_F(Test3NodeDropinSubsetPhase02, OsUserInfoCompatTest) {
  EnvScope s(runtime_.get());
  std::string error;
  const int exit_code = RunNodeCompatScript(s.env, "parallel/test-os-userinfo-handles-getter-errors.js", &error);
  EXPECT_EQ(exit_code, 0) << "error=" << error;
  EXPECT_TRUE(error.empty()) << "error=" << error;
}

TEST_F(Test3NodeDropinSubsetPhase02, OsHomedirCompatTest) {
  EnvScope s(runtime_.get());
  std::string error;
  const int exit_code = RunNodeCompatScript(s.env, "parallel/test-os-homedir-no-envvar.js", &error);
  EXPECT_EQ(exit_code, 0) << "error=" << error;
  EXPECT_TRUE(error.empty()) << "error=" << error;
}

TEST_F(Test3NodeDropinSubsetPhase02, OsConstantsCompatTest) {
  EnvScope s(runtime_.get());
  std::string error;
  const int exit_code = RunNodeCompatScript(s.env, "parallel/test-os-constants-signals.js", &error);
  EXPECT_EQ(exit_code, 0) << "error=" << error;
  EXPECT_TRUE(error.empty()) << "error=" << error;
}

TEST_F(Test3NodeDropinSubsetPhase02, OsFastCompatTest) {
  EnvScope s(runtime_.get());
  std::string error;
  const int exit_code = RunNodeCompatScript(s.env, "parallel/test-os-fast.js", &error);
  EXPECT_EQ(exit_code, 0) << "error=" << error;
  EXPECT_TRUE(error.empty()) << "error=" << error;
}

TEST_F(Test3NodeDropinSubsetPhase02, OsCheckedCompatTest) {
  EnvScope s(runtime_.get());
  std::string error;
  const int exit_code = RunNodeCompatScript(s.env, "parallel/test-os-checked-function.js", &error);
  EXPECT_EQ(exit_code, 0) << "error=" << error;
  EXPECT_TRUE(error.empty()) << "error=" << error;
}

#ifdef NAPI_V8_NODE_ROOT_PATH
TEST_F(Test3NodeDropinSubsetPhase02, RawRequireCacheFromNodeTest) {
  EnvScope s(runtime_.get());
  std::string error;
  const int exit_code = RunRawNodeTestScript(s.env, "test-require-cache.js", &error);
  EXPECT_EQ(exit_code, 0) << "error=" << error;
  EXPECT_TRUE(error.empty()) << "error=" << error;
}

TEST_F(Test3NodeDropinSubsetPhase02, RawRequireJsonFromNodeTest) {
  EnvScope s(runtime_.get());
  std::string error;
  const int exit_code = RunRawNodeTestScript(s.env, "test-require-json.js", &error);
  EXPECT_EQ(exit_code, 0) << "error=" << error;
  EXPECT_TRUE(error.empty()) << "error=" << error;
}

TEST_F(Test3NodeDropinSubsetPhase02, RawModuleCacheFromNodeTest) {
  EnvScope s(runtime_.get());
  std::string error;
  const int exit_code = RunRawNodeTestScript(s.env, "test-module-cache.js", &error);
  EXPECT_EQ(exit_code, 0) << "error=" << error;
  EXPECT_TRUE(error.empty()) << "error=" << error;
}

TEST_F(Test3NodeDropinSubsetPhase02, RawRequireDotFromNodeTest) {
  EnvScope s(runtime_.get());
  std::string error;
  const int exit_code = RunRawNodeTestScript(s.env, "test-require-dot.js", &error);
  EXPECT_EQ(exit_code, 0) << "error=" << error;
  EXPECT_TRUE(error.empty()) << "error=" << error;
}

TEST_F(Test3NodeDropinSubsetPhase02, RawFsExistsFromNodeTest) {
  EnvScope s(runtime_.get());
  std::string error;
  const int exit_code = RunRawNodeTestScript(s.env, "test-fs-exists.js", &error);
  EXPECT_EQ(exit_code, 0) << "error=" << error;
  EXPECT_TRUE(error.empty()) << "error=" << error;
}

TEST_F(Test3NodeDropinSubsetPhase02, RawFsStatFromNodeTest) {
  EnvScope s(runtime_.get());
  std::string error;
  const int exit_code = RunRawNodeTestScript(s.env, "test-fs-stat.js", &error);
  EXPECT_EQ(exit_code, 0) << "error=" << error;
  EXPECT_TRUE(error.empty()) << "error=" << error;
}

TEST_F(Test3NodeDropinSubsetPhase02, RawFsWriteSyncFromNodeTest) {
  EnvScope s(runtime_.get());
  std::string error;
  const int exit_code = RunRawNodeTestScript(s.env, "test-fs-write-sync.js", &error);
  EXPECT_EQ(exit_code, 0) << "error=" << error;
  EXPECT_TRUE(error.empty()) << "error=" << error;
}

TEST_F(Test3NodeDropinSubsetPhase02, RawFsReadFromNodeTest) {
  EnvScope s(runtime_.get());
  std::string error;
  const int exit_code = RunRawNodeTestScript(s.env, "test-fs-read.js", &error);
  EXPECT_EQ(exit_code, 0) << "error=" << error;
  EXPECT_TRUE(error.empty()) << "error=" << error;
}

TEST_F(Test3NodeDropinSubsetPhase02, RawFsReaddirFromNodeTest) {
  EnvScope s(runtime_.get());
  std::string error;
  const int exit_code = RunRawNodeTestScript(s.env, "test-fs-readdir.js", &error);
  EXPECT_EQ(exit_code, 0) << "error=" << error;
  EXPECT_TRUE(error.empty()) << "error=" << error;
}

TEST_F(Test3NodeDropinSubsetPhase02, RawFsRenameFromNodeTest) {
  EnvScope s(runtime_.get());
  std::string error;
  const int exit_code = RunRawNodeTestScript(s.env, "test-fs-rename-type-check.js", &error);
  EXPECT_EQ(exit_code, 0) << "error=" << error;
  EXPECT_TRUE(error.empty()) << "error=" << error;
}

TEST_F(Test3NodeDropinSubsetPhase02, RawFsUnlinkFromNodeTest) {
  EnvScope s(runtime_.get());
  std::string error;
  const int exit_code = RunRawNodeTestScript(s.env, "test-fs-unlink-type-check.js", &error);
  EXPECT_EQ(exit_code, 0) << "error=" << error;
  EXPECT_TRUE(error.empty()) << "error=" << error;
}

TEST_F(Test3NodeDropinSubsetPhase02, RawFsTruncateSyncFromNodeTest) {
  EnvScope s(runtime_.get());
  std::string error;
  const int exit_code = RunRawNodeTestScript(s.env, "test-fs-truncate-sync.js", &error);
  EXPECT_EQ(exit_code, 0) << "error=" << error;
  EXPECT_TRUE(error.empty()) << "error=" << error;
}

TEST_F(Test3NodeDropinSubsetPhase02, RawFsCopyfileSyncFromNodeTest) {
  EnvScope s(runtime_.get());
  std::string error;
  const int exit_code = RunRawNodeTestScript(s.env, "test-fs-copyfile.js", &error);
  EXPECT_EQ(exit_code, 0) << "error=" << error;
  EXPECT_TRUE(error.empty()) << "error=" << error;
}

TEST_F(Test3NodeDropinSubsetPhase02, RawFsAppendFileSyncFromNodeTest) {
  EnvScope s(runtime_.get());
  std::string error;
  const int exit_code = RunRawNodeTestScript(s.env, "test-fs-append-file-sync.js", &error);
  EXPECT_EQ(exit_code, 0) << "error=" << error;
  EXPECT_TRUE(error.empty()) << "error=" << error;
}

TEST_F(Test3NodeDropinSubsetPhase02, RawFsMkdtempFromNodeTest) {
  EnvScope s(runtime_.get());
  std::string error;
  const int exit_code = RunRawNodeTestScript(s.env, "test-fs-mkdtemp.js", &error);
  EXPECT_EQ(exit_code, 0) << "error=" << error;
  EXPECT_TRUE(error.empty()) << "error=" << error;
}

TEST_F(Test3NodeDropinSubsetPhase02, RawFsReadlinkTypeCheckFromNodeTest) {
  EnvScope s(runtime_.get());
  std::string error;
  const int exit_code = RunRawNodeTestScript(s.env, "test-fs-readlink-type-check.js", &error);
  EXPECT_EQ(exit_code, 0) << "error=" << error;
  EXPECT_TRUE(error.empty()) << "error=" << error;
}

TEST_F(Test3NodeDropinSubsetPhase02, RawFsSymlinkFromNodeTest) {
  EnvScope s(runtime_.get());
  std::string error;
  const int exit_code = RunRawNodeTestScript(s.env, "test-fs-symlink.js", &error);
  EXPECT_EQ(exit_code, 0) << "error=" << error;
  EXPECT_TRUE(error.empty()) << "error=" << error;
}

TEST_F(Test3NodeDropinSubsetPhase02, RawFsChmodFromNodeTest) {
  EnvScope s(runtime_.get());
  std::string error;
  const int exit_code = RunRawNodeTestScript(s.env, "test-fs-chmod.js", &error);
  EXPECT_EQ(exit_code, 0) << "error=" << error;
  EXPECT_TRUE(error.empty()) << "error=" << error;
}

TEST_F(Test3NodeDropinSubsetPhase02, RawFsUtimesFromNodeTest) {
  EnvScope s(runtime_.get());
  std::string error;
  const int exit_code = RunRawNodeTestScript(s.env, "test-fs-utimes.js", &error);
  EXPECT_EQ(exit_code, 0) << "error=" << error;
  EXPECT_TRUE(error.empty()) << "error=" << error;
}

TEST_F(Test3NodeDropinSubsetPhase02, RawOsFromNodeTest) {
  EnvScope s(runtime_.get());
  std::string error;
  const int exit_code = RunRawNodeTestScript(s.env, "test-os.js", &error);
  EXPECT_EQ(exit_code, 0) << "error=" << error;
  EXPECT_TRUE(error.empty()) << "error=" << error;
}

TEST_F(Test3NodeDropinSubsetPhase02, RawOsEolFromNodeTest) {
  EnvScope s(runtime_.get());
  std::string error;
  const int exit_code = RunRawNodeTestScript(s.env, "test-os-eol.js", &error);
  EXPECT_EQ(exit_code, 0) << "error=" << error;
  EXPECT_TRUE(error.empty()) << "error=" << error;
}

TEST_F(Test3NodeDropinSubsetPhase02, RawOsProcessPriorityFromNodeTest) {
  EnvScope s(runtime_.get());
  std::string error;
  const int exit_code = RunRawNodeTestScript(s.env, "test-os-process-priority.js", &error);
  EXPECT_EQ(exit_code, 0) << "error=" << error;
  EXPECT_TRUE(error.empty()) << "error=" << error;
}

TEST_F(Test3NodeDropinSubsetPhase02, RawOsCheckedFunctionFromNodeTest) {
  EnvScope s(runtime_.get());
  std::string error;
  const int exit_code = RunRawNodeTestScript(s.env, "test-os-checked-function.js", &error);
  EXPECT_EQ(exit_code, 0) << "error=" << error;
  EXPECT_TRUE(error.empty()) << "error=" << error;
}

TEST_F(Test3NodeDropinSubsetPhase02, RawOsFastFromNodeTest) {
  EnvScope s(runtime_.get());
  std::string error;
  const int exit_code = RunRawNodeTestScript(s.env, "test-os-fast.js", &error);
  EXPECT_EQ(exit_code, 0) << "error=" << error;
  EXPECT_TRUE(error.empty()) << "error=" << error;
}

TEST_F(Test3NodeDropinSubsetPhase02, RawOsHomedirNoEnvvarFromNodeTest) {
  EnvScope s(runtime_.get());
  std::string error;
  const int exit_code = RunRawNodeTestScript(s.env, "test-os-homedir-no-envvar.js", &error);
  EXPECT_EQ(exit_code, 0) << "error=" << error;
  EXPECT_TRUE(error.empty()) << "error=" << error;
}

TEST_F(Test3NodeDropinSubsetPhase02, RawOsUserInfoHandlesGetterErrorsFromNodeTest) {
  EnvScope s(runtime_.get());
  std::string error;
  const int exit_code = RunRawNodeTestScript(s.env, "test-os-userinfo-handles-getter-errors.js", &error);
  EXPECT_EQ(exit_code, 0) << "error=" << error;
  EXPECT_TRUE(error.empty()) << "error=" << error;
}

TEST_F(Test3NodeDropinSubsetPhase02, RawOsConstantsSignalsFromNodeTest) {
  EnvScope s(runtime_.get());
  std::string error;
  const int exit_code = RunRawNodeTestScript(s.env, "test-os-constants-signals.js", &error);
  EXPECT_EQ(exit_code, 0) << "error=" << error;
  EXPECT_TRUE(error.empty()) << "error=" << error;
}

TEST_F(Test3NodeDropinSubsetPhase02, NodeCompatEventEmitterMethodNamesTest) {
  EnvScope s(runtime_.get());
  std::string error;
  const int exit_code = RunNodeCompatScript(s.env, "parallel/test-event-emitter-method-names.js", &error);
  EXPECT_EQ(exit_code, 0) << "error=" << error;
  EXPECT_TRUE(error.empty()) << "error=" << error;
}

#define DEFINE_RAW_NODE_TEST(test_name, script_name)            \
  TEST_F(Test3NodeDropinSubsetPhase02, test_name) {             \
    EnvScope s(runtime_.get());                                 \
    std::string error;                                          \
    const int exit_code = RunRawNodeTestScript(s.env, script_name, &error); \
    EXPECT_EQ(exit_code, 0) << "error=" << error;               \
    EXPECT_TRUE(error.empty()) << "error=" << error;            \
  }

DEFINE_RAW_NODE_TEST(RawBufferAllocFromNodeTest, "test-buffer-alloc.js")
DEFINE_RAW_NODE_TEST(RawBufferArraybufferFromNodeTest, "test-buffer-arraybuffer.js")
DEFINE_RAW_NODE_TEST(RawBufferAsciiFromNodeTest, "test-buffer-ascii.js")
DEFINE_RAW_NODE_TEST(RawBufferBackingArraybufferFromNodeTest, "test-buffer-backing-arraybuffer.js")
DEFINE_RAW_NODE_TEST(RawBufferBadhexFromNodeTest, "test-buffer-badhex.js")
DEFINE_RAW_NODE_TEST(RawBufferBigint64FromNodeTest, "test-buffer-bigint64.js")
DEFINE_RAW_NODE_TEST(RawBufferBytelengthFromNodeTest, "test-buffer-bytelength.js")
DEFINE_RAW_NODE_TEST(RawBufferCompareOffsetFromNodeTest, "test-buffer-compare-offset.js")
DEFINE_RAW_NODE_TEST(RawBufferCompareFromNodeTest, "test-buffer-compare.js")
DEFINE_RAW_NODE_TEST(RawBufferConcatFromNodeTest, "test-buffer-concat.js")
DEFINE_RAW_NODE_TEST(RawBufferConstantsFromNodeTest, "test-buffer-constants.js")
DEFINE_RAW_NODE_TEST(RawBufferConstructorDeprecationErrorFromNodeTest, "test-buffer-constructor-deprecation-error.js")
DEFINE_RAW_NODE_TEST(RawBufferConstructorNodeModulesPathsFromNodeTest, "test-buffer-constructor-node-modules-paths.js")
DEFINE_RAW_NODE_TEST(RawBufferConstructorNodeModulesFromNodeTest, "test-buffer-constructor-node-modules.js")
DEFINE_RAW_NODE_TEST(RawBufferConstructorOutsideNodeModulesFromNodeTest, "test-buffer-constructor-outside-node-modules.js")
DEFINE_RAW_NODE_TEST(RawBufferCopyFromNodeTest, "test-buffer-copy.js")
DEFINE_RAW_NODE_TEST(RawBufferEqualsFromNodeTest, "test-buffer-equals.js")
DEFINE_RAW_NODE_TEST(RawBufferFailedAllocTypedArraysFromNodeTest, "test-buffer-failed-alloc-typed-arrays.js")
DEFINE_RAW_NODE_TEST(RawBufferFakesFromNodeTest, "test-buffer-fakes.js")
DEFINE_RAW_NODE_TEST(RawBufferFillFromNodeTest, "test-buffer-fill.js")
DEFINE_RAW_NODE_TEST(RawBufferFromFromNodeTest, "test-buffer-from.js")
DEFINE_RAW_NODE_TEST(RawBufferGenericMethodsFromNodeTest, "test-buffer-generic-methods.js")
DEFINE_RAW_NODE_TEST(RawBufferIncludesFromNodeTest, "test-buffer-includes.js")
DEFINE_RAW_NODE_TEST(RawBufferIndexofFromNodeTest, "test-buffer-indexof.js")
DEFINE_RAW_NODE_TEST(RawBufferInheritanceFromNodeTest, "test-buffer-inheritance.js")
DEFINE_RAW_NODE_TEST(RawBufferInspectFromNodeTest, "test-buffer-inspect.js")
DEFINE_RAW_NODE_TEST(RawBufferIsasciiFromNodeTest, "test-buffer-isascii.js")
DEFINE_RAW_NODE_TEST(RawBufferIsencodingFromNodeTest, "test-buffer-isencoding.js")
DEFINE_RAW_NODE_TEST(RawBufferIsutf8FromNodeTest, "test-buffer-isutf8.js")
DEFINE_RAW_NODE_TEST(RawBufferIteratorFromNodeTest, "test-buffer-iterator.js")
DEFINE_RAW_NODE_TEST(RawBufferNewFromNodeTest, "test-buffer-new.js")
DEFINE_RAW_NODE_TEST(RawBufferNoNegativeAllocationFromNodeTest, "test-buffer-no-negative-allocation.js")
DEFINE_RAW_NODE_TEST(RawBufferNopendingdepMapFromNodeTest, "test-buffer-nopendingdep-map.js")
DEFINE_RAW_NODE_TEST(RawBufferOfNoDeprecationFromNodeTest, "test-buffer-of-no-deprecation.js")
DEFINE_RAW_NODE_TEST(RawBufferOverMaxLengthFromNodeTest, "test-buffer-over-max-length.js")
DEFINE_RAW_NODE_TEST(RawBufferParentPropertyFromNodeTest, "test-buffer-parent-property.js")
DEFINE_RAW_NODE_TEST(RawBufferPendingDeprecationFromNodeTest, "test-buffer-pending-deprecation.js")
DEFINE_RAW_NODE_TEST(RawBufferPoolUntransferableFromNodeTest, "test-buffer-pool-untransferable.js")
DEFINE_RAW_NODE_TEST(RawBufferPrototypeInspectFromNodeTest, "test-buffer-prototype-inspect.js")
DEFINE_RAW_NODE_TEST(RawBufferReadFromNodeTest, "test-buffer-read.js")
DEFINE_RAW_NODE_TEST(RawBufferReaddoubleFromNodeTest, "test-buffer-readdouble.js")
DEFINE_RAW_NODE_TEST(RawBufferReadfloatFromNodeTest, "test-buffer-readfloat.js")
DEFINE_RAW_NODE_TEST(RawBufferReadintFromNodeTest, "test-buffer-readint.js")
DEFINE_RAW_NODE_TEST(RawBufferReaduintFromNodeTest, "test-buffer-readuint.js")
DEFINE_RAW_NODE_TEST(RawBufferResizableFromNodeTest, "test-buffer-resizable.js")
DEFINE_RAW_NODE_TEST(RawBufferSafeUnsafeFromNodeTest, "test-buffer-safe-unsafe.js")
DEFINE_RAW_NODE_TEST(RawBufferSetInspectMaxBytesFromNodeTest, "test-buffer-set-inspect-max-bytes.js")
DEFINE_RAW_NODE_TEST(RawBufferSharedarraybufferFromNodeTest, "test-buffer-sharedarraybuffer.js")
DEFINE_RAW_NODE_TEST(RawBufferSliceFromNodeTest, "test-buffer-slice.js")
DEFINE_RAW_NODE_TEST(RawBufferSlowFromNodeTest, "test-buffer-slow.js")
DEFINE_RAW_NODE_TEST(RawBufferTojsonFromNodeTest, "test-buffer-tojson.js")
DEFINE_RAW_NODE_TEST(RawBufferTostring4gbFromNodeTest, "test-buffer-tostring-4gb.js")
DEFINE_RAW_NODE_TEST(RawBufferTostringRangeFromNodeTest, "test-buffer-tostring-range.js")
DEFINE_RAW_NODE_TEST(RawBufferTostringRangeerrorFromNodeTest, "test-buffer-tostring-rangeerror.js")
DEFINE_RAW_NODE_TEST(RawBufferTostringFromNodeTest, "test-buffer-tostring.js")
DEFINE_RAW_NODE_TEST(RawBufferWriteFastFromNodeTest, "test-buffer-write-fast.js")
DEFINE_RAW_NODE_TEST(RawBufferWriteFromNodeTest, "test-buffer-write.js")
DEFINE_RAW_NODE_TEST(RawBufferWritedoubleFromNodeTest, "test-buffer-writedouble.js")
DEFINE_RAW_NODE_TEST(RawBufferWritefloatFromNodeTest, "test-buffer-writefloat.js")
DEFINE_RAW_NODE_TEST(RawBufferWriteintFromNodeTest, "test-buffer-writeint.js")
DEFINE_RAW_NODE_TEST(RawBufferWriteuintFromNodeTest, "test-buffer-writeuint.js")
DEFINE_RAW_NODE_TEST(RawBufferZeroFillCliFromNodeTest, "test-buffer-zero-fill-cli.js")
DEFINE_RAW_NODE_TEST(RawBufferZeroFillResetFromNodeTest, "test-buffer-zero-fill-reset.js")
DEFINE_RAW_NODE_TEST(RawBufferZeroFillFromNodeTest, "test-buffer-zero-fill.js")
DEFINE_RAW_NODE_TEST(RawBufferAllocUnsafeIsInitializedWithZeroFillFlagFromNodeTest, "test-buffer-alloc-unsafe-is-initialized-with-zero-fill-flag.js")
DEFINE_RAW_NODE_TEST(RawBufferAllocUnsafeIsUninitializedFromNodeTest, "test-buffer-alloc-unsafe-is-uninitialized.js")
DEFINE_RAW_NODE_TEST(RawBufferSwapFromNodeTest, "test-buffer-swap.js")

// Raw Node events/EventEmitter tests (drop-in from node/test/parallel)
DEFINE_RAW_NODE_TEST(RawEventCaptureRejectionsFromNodeTest, "test-event-capture-rejections.js")
DEFINE_RAW_NODE_TEST(RawEventEmitterAddListenersFromNodeTest, "test-event-emitter-add-listeners.js")
DEFINE_RAW_NODE_TEST(RawEventEmitterCheckListenerLeaksFromNodeTest, "test-event-emitter-check-listener-leaks.js")
DEFINE_RAW_NODE_TEST(RawEventEmitterEmitContextFromNodeTest, "test-event-emitter-emit-context.js")
DEFINE_RAW_NODE_TEST(RawEventEmitterErrorMonitorFromNodeTest, "test-event-emitter-error-monitor.js")
DEFINE_RAW_NODE_TEST(RawEventEmitterErrorsFromNodeTest, "test-event-emitter-errors.js")
DEFINE_RAW_NODE_TEST(RawEventEmitterGetMaxListenersFromNodeTest, "test-event-emitter-get-max-listeners.js")
DEFINE_RAW_NODE_TEST(RawEventEmitterInvalidListenerFromNodeTest, "test-event-emitter-invalid-listener.js")
DEFINE_RAW_NODE_TEST(RawEventEmitterListenerCountFromNodeTest, "test-event-emitter-listener-count.js")
DEFINE_RAW_NODE_TEST(RawEventEmitterListenersFromNodeTest, "test-event-emitter-listeners.js")
DEFINE_RAW_NODE_TEST(RawEventEmitterListenersSideEffectsFromNodeTest, "test-event-emitter-listeners-side-effects.js")
DEFINE_RAW_NODE_TEST(RawEventEmitterMaxListenersFromNodeTest, "test-event-emitter-max-listeners.js")
DEFINE_RAW_NODE_TEST(RawEventEmitterMaxListenersWarningFromNodeTest, "test-event-emitter-max-listeners-warning.js")
DEFINE_RAW_NODE_TEST(RawEventEmitterMaxListenersWarningForSymbolFromNodeTest, "test-event-emitter-max-listeners-warning-for-symbol.js")
DEFINE_RAW_NODE_TEST(RawEventEmitterMaxListenersWarningForNullFromNodeTest, "test-event-emitter-max-listeners-warning-for-null.js")
DEFINE_RAW_NODE_TEST(RawEventEmitterModifyInEmitFromNodeTest, "test-event-emitter-modify-in-emit.js")
DEFINE_RAW_NODE_TEST(RawEventEmitterNoErrorProvidedToErrorEventFromNodeTest, "test-event-emitter-no-error-provided-to-error-event.js")
DEFINE_RAW_NODE_TEST(RawEventEmitterNumArgsFromNodeTest, "test-event-emitter-num-args.js")
DEFINE_RAW_NODE_TEST(RawEventEmitterOnceFromNodeTest, "test-event-emitter-once.js")
DEFINE_RAW_NODE_TEST(RawEventEmitterPrependFromNodeTest, "test-event-emitter-prepend.js")
DEFINE_RAW_NODE_TEST(RawEventEmitterRemoveAllListenersFromNodeTest, "test-event-emitter-remove-all-listeners.js")
DEFINE_RAW_NODE_TEST(RawEventEmitterRemoveListenersFromNodeTest, "test-event-emitter-remove-listeners.js")
DEFINE_RAW_NODE_TEST(RawEventEmitterSetMaxListenersSideEffectsFromNodeTest, "test-event-emitter-set-max-listeners-side-effects.js")
DEFINE_RAW_NODE_TEST(RawEventEmitterSpecialEventNamesFromNodeTest, "test-event-emitter-special-event-names.js")
DEFINE_RAW_NODE_TEST(RawEventEmitterSubclassFromNodeTest, "test-event-emitter-subclass.js")
DEFINE_RAW_NODE_TEST(RawEventEmitterSymbolsFromNodeTest, "test-event-emitter-symbols.js")
DEFINE_RAW_NODE_TEST(RawEventsCustomeventFromNodeTest, "test-events-customevent.js")
DEFINE_RAW_NODE_TEST(RawEventsGetmaxlistenersFromNodeTest, "test-events-getmaxlisteners.js")
DEFINE_RAW_NODE_TEST(RawEventsListFromNodeTest, "test-events-list.js")
DEFINE_RAW_NODE_TEST(RawEventsListenerCountWithListenerFromNodeTest, "test-events-listener-count-with-listener.js")
DEFINE_RAW_NODE_TEST(RawEventsOnAsyncIteratorFromNodeTest, "test-events-on-async-iterator.js")
DEFINE_RAW_NODE_TEST(RawEventsOnceFromNodeTest, "test-events-once.js")
DEFINE_RAW_NODE_TEST(RawEventsStaticGeteventlistenersFromNodeTest, "test-events-static-geteventlisteners.js")
DEFINE_RAW_NODE_TEST(RawEventsUncaughtExceptionStackFromNodeTest, "test-events-uncaught-exception-stack.js")
DEFINE_RAW_NODE_TEST(RawEventTargetFromNodeTest, "test-event-target.js")

#undef DEFINE_RAW_NODE_TEST

TEST_F(Test3NodeDropinSubsetPhase02, RawPathFromNodeTest) {
  EnvScope s(runtime_.get());
  std::string error;
  const int exit_code = RunRawNodeTestScript(s.env, "test-path.js", &error);
  EXPECT_EQ(exit_code, 0) << "error=" << error;
  EXPECT_TRUE(error.empty()) << "error=" << error;
}

TEST_F(Test3NodeDropinSubsetPhase02, RawPathBasenameFromNodeTest) {
  EnvScope s(runtime_.get());
  std::string error;
  const int exit_code = RunRawNodeTestScript(s.env, "test-path-basename.js", &error);
  EXPECT_EQ(exit_code, 0) << "error=" << error;
  EXPECT_TRUE(error.empty()) << "error=" << error;
}

TEST_F(Test3NodeDropinSubsetPhase02, RawPathDirnameFromNodeTest) {
  EnvScope s(runtime_.get());
  std::string error;
  const int exit_code = RunRawNodeTestScript(s.env, "test-path-dirname.js", &error);
  EXPECT_EQ(exit_code, 0) << "error=" << error;
  EXPECT_TRUE(error.empty()) << "error=" << error;
}

TEST_F(Test3NodeDropinSubsetPhase02, RawPathExtnameFromNodeTest) {
  EnvScope s(runtime_.get());
  std::string error;
  const int exit_code = RunRawNodeTestScript(s.env, "test-path-extname.js", &error);
  EXPECT_EQ(exit_code, 0) << "error=" << error;
  EXPECT_TRUE(error.empty()) << "error=" << error;
}

TEST_F(Test3NodeDropinSubsetPhase02, RawPathGlobFromNodeTest) {
  EnvScope s(runtime_.get());
  std::string error;
  const int exit_code = RunRawNodeTestScript(s.env, "test-path-glob.js", &error);
  EXPECT_EQ(exit_code, 0) << "error=" << error;
  EXPECT_TRUE(error.empty()) << "error=" << error;
}

TEST_F(Test3NodeDropinSubsetPhase02, RawPathIsAbsoluteFromNodeTest) {
  EnvScope s(runtime_.get());
  std::string error;
  const int exit_code = RunRawNodeTestScript(s.env, "test-path-isabsolute.js", &error);
  EXPECT_EQ(exit_code, 0) << "error=" << error;
  EXPECT_TRUE(error.empty()) << "error=" << error;
}

TEST_F(Test3NodeDropinSubsetPhase02, RawPathJoinFromNodeTest) {
  EnvScope s(runtime_.get());
  std::string error;
  const int exit_code = RunRawNodeTestScript(s.env, "test-path-join.js", &error);
  EXPECT_EQ(exit_code, 0) << "error=" << error;
  EXPECT_TRUE(error.empty()) << "error=" << error;
}

TEST_F(Test3NodeDropinSubsetPhase02, RawPathMakeLongFromNodeTest) {
  EnvScope s(runtime_.get());
  std::string error;
  const int exit_code = RunRawNodeTestScript(s.env, "test-path-makelong.js", &error);
  EXPECT_EQ(exit_code, 0) << "error=" << error;
  EXPECT_TRUE(error.empty()) << "error=" << error;
}

TEST_F(Test3NodeDropinSubsetPhase02, RawPathNormalizeFromNodeTest) {
  EnvScope s(runtime_.get());
  std::string error;
  const int exit_code = RunRawNodeTestScript(s.env, "test-path-normalize.js", &error);
  EXPECT_EQ(exit_code, 0) << "error=" << error;
  EXPECT_TRUE(error.empty()) << "error=" << error;
}

TEST_F(Test3NodeDropinSubsetPhase02, RawPathParseFormatFromNodeTest) {
  EnvScope s(runtime_.get());
  std::string error;
  const int exit_code = RunRawNodeTestScript(s.env, "test-path-parse-format.js", &error);
  EXPECT_EQ(exit_code, 0) << "error=" << error;
  EXPECT_TRUE(error.empty()) << "error=" << error;
}

TEST_F(Test3NodeDropinSubsetPhase02, RawPathPosixExistsFromNodeTest) {
  EnvScope s(runtime_.get());
  std::string error;
  const int exit_code = RunRawNodeTestScript(s.env, "test-path-posix-exists.js", &error);
  EXPECT_EQ(exit_code, 0) << "error=" << error;
  EXPECT_TRUE(error.empty()) << "error=" << error;
}

TEST_F(Test3NodeDropinSubsetPhase02, RawPathPosixRelativeOnWindowsFromNodeTest) {
  EnvScope s(runtime_.get());
  std::string error;
  const int exit_code = RunRawNodeTestScript(s.env, "test-path-posix-relative-on-windows.js", &error);
  EXPECT_EQ(exit_code, 0) << "error=" << error;
  EXPECT_TRUE(error.empty()) << "error=" << error;
}

TEST_F(Test3NodeDropinSubsetPhase02, RawPathRelativeFromNodeTest) {
  EnvScope s(runtime_.get());
  std::string error;
  const int exit_code = RunRawNodeTestScript(s.env, "test-path-relative.js", &error);
  EXPECT_EQ(exit_code, 0) << "error=" << error;
  EXPECT_TRUE(error.empty()) << "error=" << error;
}

TEST_F(Test3NodeDropinSubsetPhase02, RawPathResolveFromNodeTest) {
  EnvScope s(runtime_.get());
  std::string error;
  const int exit_code = RunRawNodeTestScript(s.env, "test-path-resolve.js", &error);
  EXPECT_EQ(exit_code, 0) << "error=" << error;
  EXPECT_TRUE(error.empty()) << "error=" << error;
}

TEST_F(Test3NodeDropinSubsetPhase02, RawPathWin32ExistsFromNodeTest) {
  EnvScope s(runtime_.get());
  std::string error;
  const int exit_code = RunRawNodeTestScript(s.env, "test-path-win32-exists.js", &error);
  EXPECT_EQ(exit_code, 0) << "error=" << error;
  EXPECT_TRUE(error.empty()) << "error=" << error;
}

TEST_F(Test3NodeDropinSubsetPhase02, RawPathWin32NormalizeDeviceNamesFromNodeTest) {
  EnvScope s(runtime_.get());
  std::string error;
  const int exit_code = RunRawNodeTestScript(s.env, "test-path-win32-normalize-device-names.js", &error);
  EXPECT_EQ(exit_code, 0) << "error=" << error;
  EXPECT_TRUE(error.empty()) << "error=" << error;
}

TEST_F(Test3NodeDropinSubsetPhase02, RawPathZeroLengthStringsFromNodeTest) {
  EnvScope s(runtime_.get());
  std::string error;
  const int exit_code = RunRawNodeTestScript(s.env, "test-path-zero-length-strings.js", &error);
  EXPECT_EQ(exit_code, 0) << "error=" << error;
  EXPECT_TRUE(error.empty()) << "error=" << error;
}
#endif
