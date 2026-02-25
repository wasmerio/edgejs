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
#endif
