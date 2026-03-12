#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <cstring>
#include <errno.h>
#include <process.h>
#include <windows.h>
#else
#include <cerrno>
#include <cstring>
#include <unistd.h>
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif
#endif

#include "edge_cli.h"

namespace {

std::string FallbackExecPath(const char* argv0) {
  namespace fs = std::filesystem;
  if (argv0 != nullptr && argv0[0] != '\0') {
    const fs::path candidate(argv0);
    if (candidate.is_absolute()) {
      return candidate.lexically_normal().string();
    }
    std::error_code ec;
    if (candidate.has_parent_path()) {
      return fs::absolute(candidate, ec).lexically_normal().string();
    }

    const char* path_env = std::getenv("PATH");
    if (path_env != nullptr && path_env[0] != '\0') {
#if defined(_WIN32)
      constexpr char kPathSeparator = ';';
      const char* exe_suffix = ".exe";
#else
      constexpr char kPathSeparator = ':';
#endif
      std::string path_value(path_env);
      size_t start = 0;
      while (start <= path_value.size()) {
        const size_t end = path_value.find(kPathSeparator, start);
        const std::string entry =
            path_value.substr(start, end == std::string::npos ? std::string::npos : end - start);
        if (!entry.empty()) {
          const fs::path base(entry);
          std::vector<fs::path> candidates = {base / candidate};
#if defined(_WIN32)
          candidates.push_back(base / (candidate.string() + exe_suffix));
#endif
          for (const auto& path_candidate : candidates) {
            ec.clear();
            if (fs::exists(path_candidate, ec) && !ec && !fs::is_directory(path_candidate, ec)) {
              return fs::absolute(path_candidate, ec).lexically_normal().string();
            }
          }
        }
        if (end == std::string::npos) break;
        start = end + 1;
      }
    }
    return candidate.string();
  }
  return "edgeenv";
}

std::string DetectActualExecPath(const char* argv0) {
#if defined(_WIN32)
  std::vector<char> buffer(MAX_PATH, '\0');
  DWORD length = GetModuleFileNameA(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
  while (length == buffer.size()) {
    buffer.resize(buffer.size() * 2, '\0');
    length = GetModuleFileNameA(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
  }
  if (length > 0) {
    return std::string(buffer.data(), length);
  }
#elif defined(__APPLE__)
  uint32_t size = 0;
  _NSGetExecutablePath(nullptr, &size);
  if (size > 0) {
    std::vector<char> buffer(size + 1, '\0');
    if (_NSGetExecutablePath(buffer.data(), &size) == 0) {
      return std::string(buffer.data());
    }
  }
#elif defined(__linux__)
  std::vector<char> buffer(4096, '\0');
  const ssize_t length = readlink("/proc/self/exe", buffer.data(), buffer.size() - 1);
  if (length > 0) {
    buffer[static_cast<size_t>(length)] = '\0';
    return std::string(buffer.data());
  }
#endif
  return FallbackExecPath(argv0);
}

std::string DetectLogicalExecPath(const std::string& actual_exec_path) {
  const char* forced_exec = std::getenv("EDGE_EXEC_PATH");
  if (forced_exec != nullptr && forced_exec[0] != '\0') {
    return forced_exec;
  }
  return actual_exec_path;
}

std::string ResolveEdgeBinaryPath(const std::string& actual_exec_path,
                                  const std::string& logical_exec_path) {
  namespace fs = std::filesystem;
  std::vector<fs::path> candidate_dirs;
  if (!actual_exec_path.empty()) {
    candidate_dirs.push_back(fs::path(actual_exec_path).parent_path());
  }
  if (!logical_exec_path.empty()) {
    const fs::path logical_dir = fs::path(logical_exec_path).parent_path();
    if (logical_dir != fs::path(actual_exec_path).parent_path()) {
      candidate_dirs.push_back(logical_dir);
    }
  }

#if defined(_WIN32)
  constexpr const char* kEdgeBinaryName = "edge.exe";
#else
  constexpr const char* kEdgeBinaryName = "edge";
#endif

  std::error_code ec;
  for (const auto& dir : candidate_dirs) {
    if (dir.empty()) continue;
    const fs::path candidate = (dir / kEdgeBinaryName).lexically_normal();
    ec.clear();
    if (!fs::exists(candidate, ec) || ec) continue;
    ec.clear();
    if (fs::is_directory(candidate, ec) || ec) continue;
    return candidate.string();
  }

  const fs::path fallback_dir =
      !candidate_dirs.empty() ? candidate_dirs.front() : fs::path(".");
  return (fallback_dir / kEdgeBinaryName).lexically_normal().string();
}

bool SetEdgeExecPath(const std::string& logical_exec_path, std::string* error_out) {
#if defined(_WIN32)
  if (_putenv_s("EDGE_EXEC_PATH", logical_exec_path.c_str()) == 0) {
    return true;
  }
  if (error_out != nullptr) {
    *error_out = "failed to set EDGE_EXEC_PATH";
  }
  return false;
#else
  if (setenv("EDGE_EXEC_PATH", logical_exec_path.c_str(), 1) == 0) {
    return true;
  }
  if (error_out != nullptr) {
    *error_out = std::string("failed to set EDGE_EXEC_PATH: ") + std::strerror(errno);
  }
  return false;
#endif
}

}  // namespace

int main(int argc, char** argv) {
  const std::string actual_exec_path = DetectActualExecPath(argc > 0 ? argv[0] : nullptr);
  const std::string logical_exec_path = DetectLogicalExecPath(actual_exec_path);
  const std::string edge_binary_path = ResolveEdgeBinaryPath(actual_exec_path, logical_exec_path);

  std::string error;
  if (!SetEdgeExecPath(logical_exec_path, &error)) {
    std::cerr << error << "\n";
    return 1;
  }

  std::vector<std::string> child_args;
  child_args.reserve(static_cast<size_t>(argc) + 1);
  child_args.push_back(argc > 0 && argv != nullptr && argv[0] != nullptr ? argv[0] : logical_exec_path);
  child_args.emplace_back(kEdgeInternalEnvCliDispatchFlag);
  for (int i = 1; i < argc; ++i) {
    if (argv[i] != nullptr) child_args.emplace_back(argv[i]);
  }

  std::vector<char*> child_argv;
  child_argv.reserve(child_args.size() + 1);
  for (auto& arg : child_args) {
    child_argv.push_back(arg.data());
  }
  child_argv.push_back(nullptr);

#if defined(_WIN32)
  _execv(edge_binary_path.c_str(), child_argv.data());
  std::cerr << "failed to exec edge: " << std::strerror(errno) << "\n";
  return errno == ENOENT ? 127 : 1;
#else
  execv(edge_binary_path.c_str(), child_argv.data());
  std::cerr << "failed to exec edge: " << std::strerror(errno) << "\n";
  return errno == ENOENT ? 127 : 1;
#endif
}
