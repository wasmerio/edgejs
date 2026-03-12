#include "builtin_catalog.h"
#include "edge_process.h"

#include <algorithm>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

namespace builtin_catalog {

namespace fs = std::filesystem;

namespace {

constexpr const char* kNodePrefix = "node:";
constexpr const char* kInternalDepsPrefix = "internal/deps/";

const std::vector<std::string> kNodeDepsBuiltinRoots = {
    "undici",
    "acorn",
    "minimatch",
    "cjs-module-lexer",
    "amaro",
    "v8/tools",
};

bool PathExistsRegularFile(const fs::path& path) {
  std::error_code ec;
  return fs::exists(path, ec) && fs::is_regular_file(path, ec);
}

bool PathExistsDirectory(const fs::path& path) {
  std::error_code ec;
  return fs::exists(path, ec) && fs::is_directory(path, ec);
}

bool ResolveAsFile(const fs::path& candidate, fs::path* out) {
  if (out == nullptr) return false;
  if (PathExistsRegularFile(candidate)) {
    *out = fs::absolute(candidate).lexically_normal();
    return true;
  }
  if (candidate.has_extension()) return false;
  const fs::path js_candidate = candidate.string() + ".js";
  if (PathExistsRegularFile(js_candidate)) {
    *out = fs::absolute(js_candidate).lexically_normal();
    return true;
  }
  const fs::path mjs_candidate = candidate.string() + ".mjs";
  if (PathExistsRegularFile(mjs_candidate)) {
    *out = fs::absolute(mjs_candidate).lexically_normal();
    return true;
  }
  return false;
}

bool IsInsideRoot(const fs::path& path, const fs::path& root) {
  const fs::path normalized_path = fs::absolute(path).lexically_normal();
  const fs::path normalized_root = fs::absolute(root).lexically_normal();
  const fs::path rel = normalized_path.lexically_relative(normalized_root);
  const std::string rel_text = rel.generic_string();
  return !rel_text.empty() && rel_text != "." && rel_text.rfind("..", 0) != 0;
}

std::string StripBuiltinExtension(const fs::path& relative_path) {
  std::string text = relative_path.generic_string();
  if (text.size() >= 4 && text.compare(text.size() - 3, 3, ".js") == 0) {
    text.resize(text.size() - 3);
    return text;
  }
  if (text.size() >= 5 && text.compare(text.size() - 4, 4, ".mjs") == 0) {
    text.resize(text.size() - 4);
    return text;
  }
  return "";
}

bool IsAllowedNodeDepsRelativePath(const fs::path& rel) {
  const std::string rel_text = rel.generic_string();
  for (const std::string& root : kNodeDepsBuiltinRoots) {
    if (rel_text == root || rel_text.rfind(root + "/", 0) == 0) return true;
  }
  return false;
}

void AppendPathCandidate(std::vector<fs::path>* out, const fs::path& candidate) {
  if (out == nullptr || candidate.empty()) return;
  out->push_back(candidate);
}

std::vector<fs::path> NodeLibRootCandidates() {
  const fs::path source_root = fs::absolute(fs::path(__FILE__).parent_path() / "..").lexically_normal();
  std::vector<fs::path> candidates;

  const fs::path exec_path = fs::path(EdgeGetProcessExecPath()).lexically_normal();
  if (!exec_path.empty()) {
    const fs::path install_root = exec_path.parent_path().parent_path();
    AppendPathCandidate(&candidates, install_root / "lib");
  }

  AppendPathCandidate(&candidates, source_root / "lib");

  std::error_code ec;
  const fs::path cwd = fs::current_path(ec);
  if (!ec && !cwd.empty()) {
    AppendPathCandidate(&candidates, cwd / "lib");
    AppendPathCandidate(&candidates, cwd.parent_path() / "lib");
  }

  return candidates;
}

std::vector<fs::path> NodeDepsRootCandidates() {
  const fs::path source_root = fs::absolute(fs::path(__FILE__).parent_path() / "..").lexically_normal();
  std::vector<fs::path> candidates;

  const fs::path exec_path = fs::path(EdgeGetProcessExecPath()).lexically_normal();
  if (!exec_path.empty()) {
    const fs::path install_root = exec_path.parent_path().parent_path();
    AppendPathCandidate(&candidates, install_root / "lib" / "internal" / "deps");
    AppendPathCandidate(&candidates, install_root / "node" / "deps");
  }

  AppendPathCandidate(&candidates, source_root / "node" / "deps");

  std::error_code ec;
  const fs::path cwd = fs::current_path(ec);
  if (!ec && !cwd.empty()) {
    AppendPathCandidate(&candidates, cwd / "node" / "deps");
    AppendPathCandidate(&candidates, cwd.parent_path() / "node" / "deps");
  }
  return candidates;
}

void AppendNodeLibIds(const fs::path& node_lib_root, std::vector<std::string>* ids) {
  if (ids == nullptr || !PathExistsDirectory(node_lib_root)) return;
  std::error_code ec;
  for (fs::recursive_directory_iterator it(node_lib_root, ec), end; it != end && !ec; it.increment(ec)) {
    if (!it->is_regular_file(ec)) continue;
    const fs::path path = it->path();
    if (path.extension() != ".js") continue;
    const fs::path rel = path.lexically_relative(node_lib_root);
    const std::string id = StripBuiltinExtension(rel);
    if (!id.empty()) ids->push_back(id);
  }
}

void AppendNodeDepsIds(const fs::path& node_deps_root, std::vector<std::string>* ids) {
  if (ids == nullptr || !PathExistsDirectory(node_deps_root)) return;
  std::error_code ec;
  for (const std::string& dep_root : kNodeDepsBuiltinRoots) {
    const fs::path root = node_deps_root / dep_root;
    if (!PathExistsDirectory(root) && !PathExistsRegularFile(root)) continue;
    if (PathExistsRegularFile(root)) {
      if (root.extension() == ".js" || root.extension() == ".mjs") {
        const fs::path rel = root.lexically_relative(node_deps_root);
        const std::string dep_id = StripBuiltinExtension(rel);
        if (!dep_id.empty()) ids->push_back(std::string(kInternalDepsPrefix) + dep_id);
      }
      continue;
    }
    for (fs::recursive_directory_iterator it(root, ec), end; it != end && !ec; it.increment(ec)) {
      if (!it->is_regular_file(ec)) continue;
      const fs::path path = it->path();
      if (path.extension() != ".js" && path.extension() != ".mjs") continue;
      const fs::path rel = path.lexically_relative(node_deps_root);
      const std::string dep_id = StripBuiltinExtension(rel);
      if (dep_id.empty()) continue;
      ids->push_back(std::string(kInternalDepsPrefix) + dep_id);
    }
  }
}

}  // namespace

const fs::path& NodeLibRoot() {
  static const fs::path root = []() {
    const std::vector<fs::path> candidates = NodeLibRootCandidates();
    for (const fs::path& candidate : candidates) {
      if (PathExistsDirectory(candidate)) return fs::absolute(candidate).lexically_normal();
    }
    if (!candidates.empty()) return fs::absolute(candidates.front()).lexically_normal();
    return fs::path();
  }();
  return root;
}

const fs::path& NodeDepsRoot() {
  static const fs::path root = []() {
    const std::vector<fs::path> candidates = NodeDepsRootCandidates();
    for (const fs::path& candidate : candidates) {
      if (PathExistsDirectory(candidate)) return fs::absolute(candidate).lexically_normal();
    }
    if (!candidates.empty()) return fs::absolute(candidates.front()).lexically_normal();
    return fs::path();
  }();
  return root;
}

bool ResolveBuiltinId(const std::string& specifier, fs::path* out_path) {
  if (out_path == nullptr) return false;
  std::string id = specifier;
  if (id.size() > 5 && id.compare(0, 5, kNodePrefix) == 0) {
    id = id.substr(5);
  }
  if (id.empty() || id.rfind(".", 0) == 0) return false;

  fs::path resolved;
  if (id.rfind(kInternalDepsPrefix, 0) == 0) {
    const std::string rel = id.substr(std::string(kInternalDepsPrefix).size());
    if (rel.empty()) return false;
    const fs::path candidate = NodeDepsRoot() / rel;
    if (ResolveAsFile(candidate, &resolved)) {
      const fs::path relative = resolved.lexically_relative(NodeDepsRoot());
      if (!IsAllowedNodeDepsRelativePath(relative)) return false;
      *out_path = resolved;
      return true;
    }
    return false;
  }

  const fs::path candidate = NodeLibRoot() / id;
  if (!ResolveAsFile(candidate, &resolved)) return false;
  if (resolved.extension() != ".js") return false;
  *out_path = resolved;
  return true;
}

bool TryGetBuiltinIdForPath(const fs::path& resolved_path, std::string* out_id) {
  if (out_id == nullptr) return false;
  const fs::path normalized = fs::absolute(resolved_path).lexically_normal();
  if (normalized.extension() != ".js" && normalized.extension() != ".mjs") return false;

  if (IsInsideRoot(normalized, NodeLibRoot())) {
    const fs::path rel = normalized.lexically_relative(NodeLibRoot());
    const std::string id = StripBuiltinExtension(rel);
    if (id.empty()) return false;
    *out_id = id;
    return true;
  }

  if (IsInsideRoot(normalized, NodeDepsRoot())) {
    const fs::path rel = normalized.lexically_relative(NodeDepsRoot());
    if (!IsAllowedNodeDepsRelativePath(rel)) return false;
    const std::string dep_id = StripBuiltinExtension(rel);
    if (dep_id.empty()) return false;
    *out_id = std::string(kInternalDepsPrefix) + dep_id;
    return true;
  }

  return false;
}

const std::vector<std::string>& AllBuiltinIds() {
  static const std::vector<std::string> ids = []() {
    std::vector<std::string> out;
    AppendNodeLibIds(NodeLibRoot(), &out);
    AppendNodeDepsIds(NodeDepsRoot(), &out);
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
  }();
  return ids;
}

}  // namespace builtin_catalog
