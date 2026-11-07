#include "builtin_catalog.h"

#include "edge_builtin_catalog_data.h"
#include "edge_process.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace builtin_catalog {

namespace fs = std::filesystem;

namespace {

constexpr std::string_view kNodePrefix = "node:";
constexpr std::string_view kInternalDepsPrefix = "internal/deps/";
constexpr const char kNodeLibRootEnvVar[] = "EDGEJS_LIB_DIR";

bool PathExistsDirectory(const fs::path& path) {
  std::error_code ec;
  return fs::exists(path, ec) && fs::is_directory(path, ec);
}

void AppendPathCandidate(std::vector<fs::path>* out, const fs::path& candidate) {
  if (out == nullptr || candidate.empty()) return;
  out->push_back(candidate);
}

void AppendEnvPathCandidate(std::vector<fs::path>* out, const char* env_var) {
  if (out == nullptr || env_var == nullptr || *env_var == '\0') return;
  const char* value = std::getenv(env_var);
  if (value == nullptr || *value == '\0') return;
  AppendPathCandidate(out, fs::path(value));
}

// Builds without embedded sources (EDGE_EMBED_BUILTIN_SOURCES=OFF, the WASIX
// packages) read every builtin from one lib root: EDGEJS_LIB_DIR when set,
// otherwise the package's /node-lib mount. There is deliberately no cwd-,
// source- or exec-relative fallback, so a user's own lib/ or deps/ directory
// is never loaded as builtin code.
constexpr const char kFileModeDefaultLibRoot[] = "/node-lib";

const char* ExplicitLibRoot() {
  const char* value = std::getenv(kNodeLibRootEnvVar);
  if (value == nullptr || *value == '\0') return nullptr;
  return value;
}

std::vector<fs::path> NodeLibRootCandidates() {
  if constexpr (!generated::kBuiltinSourcesEmbedded) {
    const char* explicit_root = ExplicitLibRoot();
    return {fs::path(explicit_root != nullptr ? explicit_root : kFileModeDefaultLibRoot)};
  }

  const fs::path source_root =
      fs::absolute(fs::path(__FILE__).parent_path() / "..").lexically_normal();
  std::vector<fs::path> candidates;

  AppendEnvPathCandidate(&candidates, kNodeLibRootEnvVar);
  AppendPathCandidate(&candidates, fs::path("/node-lib"));

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
  if constexpr (!generated::kBuiltinSourcesEmbedded) {
    // The stager mirrors Node's lib/internal/deps layout under the lib root.
    return {NodeLibRoot() / "internal" / "deps"};
  }

  const fs::path source_root =
      fs::absolute(fs::path(__FILE__).parent_path() / "..").lexically_normal();
  std::vector<fs::path> candidates;

  const char* node_lib_root_env = std::getenv(kNodeLibRootEnvVar);
  if (node_lib_root_env != nullptr && *node_lib_root_env != '\0') {
    AppendPathCandidate(&candidates, fs::path(node_lib_root_env) / "internal" / "deps");
  }

  const fs::path exec_path = fs::path(EdgeGetProcessExecPath()).lexically_normal();
  if (!exec_path.empty()) {
    const fs::path install_root = exec_path.parent_path().parent_path();
    AppendPathCandidate(&candidates, install_root / "lib" / "internal" / "deps");
    AppendPathCandidate(&candidates, install_root / "node" / "deps");
  }

  AppendPathCandidate(&candidates, source_root / "deps");

  std::error_code ec;
  const fs::path cwd = fs::current_path(ec);
  if (!ec && !cwd.empty()) {
    AppendPathCandidate(&candidates, cwd / "deps");
    AppendPathCandidate(&candidates, cwd.parent_path() / "deps");
  }
  return candidates;
}

std::string NormalizePathKey(const fs::path& path) {
  std::error_code ec;
  return fs::absolute(path, ec).lexically_normal().string();
}

fs::path EntryPath(const generated::BuiltinEntry& entry);

const char* ErrnoName(int error) {
  switch (error) {
    case ENOENT: return "ENOENT";
    case EACCES: return "EACCES";
    case EPERM: return "EPERM";
    case EISDIR: return "EISDIR";
    case ENOTDIR: return "ENOTDIR";
    case ELOOP: return "ELOOP";
    case ENAMETOOLONG: return "ENAMETOOLONG";
    case EMFILE: return "EMFILE";
    case ENFILE: return "ENFILE";
    case ENOMEM: return "ENOMEM";
    case EIO: return "EIO";
    default: return nullptr;
  }
}

// "ENOENT: No such file or directory", or "errno 123: ..." for codes without a
// name above.
std::string DescribeErrno(int error) {
  const char* name = ErrnoName(error);
  std::string out = name != nullptr ? std::string(name) : "errno " + std::to_string(error);
  out += ": ";
  out += std::strerror(error);
  return out;
}

// Reads a whole file. On failure returns false and stores the errno (EIO when
// the C library did not set one).
bool ReadWholeFile(const fs::path& path, std::string* out, int* out_errno) {
  errno = 0;
  std::FILE* file = std::fopen(path.c_str(), "rb");
  if (file == nullptr) {
    *out_errno = errno != 0 ? errno : EIO;
    return false;
  }
  std::string data;
  std::error_code size_ec;
  const auto size = fs::file_size(path, size_ec);
  if (!size_ec) data.reserve(static_cast<size_t>(size));
  char buffer[64 * 1024];
  size_t read = 0;
  while ((read = std::fread(buffer, 1, sizeof(buffer), file)) > 0) {
    data.append(buffer, read);
  }
  const bool failed = std::ferror(file) != 0;
  const int read_errno = errno;
  std::fclose(file);
  if (failed) {
    *out_errno = read_errno != 0 ? read_errno : EIO;
    return false;
  }
  *out = std::move(data);
  return true;
}

// Returns a catalog entry's source: the embedded copy, or the file under the
// lib/deps root. WASIX builds do not embed sources, so the ~10 MB of builtin
// JS stays out of every instance's linear memory, as native Node keeps it in
// its read-only executable image.
SourceReadStatus ReadEntrySource(const generated::BuiltinEntry& entry,
                                 std::string* out,
                                 std::string* error_out) {
  if constexpr (generated::kBuiltinSourcesEmbedded) {
    *out = std::string(entry.source);
    return SourceReadStatus::kOk;
  }
  const fs::path path = EntryPath(entry);
  int read_errno = 0;
  if (ReadWholeFile(path, out, &read_errno)) return SourceReadStatus::kOk;
  if (error_out != nullptr) {
    *error_out = "Cannot read built-in module '" + std::string(entry.id) + "' from " +
                 path.string() + ": " + DescribeErrno(read_errno);
  }
  return SourceReadStatus::kIoError;
}

struct BuiltinIndex {
  std::vector<std::string> all_ids;
  std::unordered_map<std::string, const generated::BuiltinEntry*> by_id;
  std::unordered_map<std::string, const generated::BuiltinEntry*> by_path;
  BuiltinCategories categories;
};

BuiltinCategories BuildBuiltinCategories(const std::vector<std::string>& ids) {
  std::unordered_set<std::string> can_be_required = {
      "internal/deps/cjs-module-lexer/lexer",
  };
  std::unordered_set<std::string> cannot_be_required = {
      "quic",
      "sqlite",
      "sys",
      "wasi",
      "internal/quic/quic",
      "internal/quic/symbols",
      "internal/quic/stats",
      "internal/quic/state",
      "internal/test/binding",
      "internal/v8_prof_polyfill",
      "internal/v8_prof_processor",
      "internal/webstorage",
  };

  std::vector<std::string_view> internal_prefixes = {
      "internal/bootstrap/",
      "internal/per_context/",
      "internal/deps/",
      "internal/main/",
  };

  cannot_be_required.insert("internal/util/inspector");
  cannot_be_required.insert("internal/inspector/network");
  cannot_be_required.insert("internal/inspector/network_http");
  cannot_be_required.insert("internal/inspector/network_http2");
  cannot_be_required.insert("internal/inspector/network_resources");
  cannot_be_required.insert("internal/inspector/network_undici");
  cannot_be_required.insert("internal/inspector_async_hook");
  cannot_be_required.insert("internal/inspector_network_tracking");

#if !defined(EDGE_BUNDLED_NAPI_V8) || !defined(EDGE_HAS_ICU)
  cannot_be_required.insert("trace_events");
#endif

  for (const std::string& id : ids) {
    for (std::string_view prefix : internal_prefixes) {
      if (id.starts_with(prefix) && can_be_required.count(id) == 0) {
        cannot_be_required.insert(id);
      }
    }
  }

  BuiltinCategories categories;
  categories.can_be_required.reserve(ids.size());
  categories.cannot_be_required.reserve(ids.size());
  for (const std::string& id : ids) {
    if (cannot_be_required.count(id) != 0) {
      categories.cannot_be_required.push_back(id);
    } else {
      categories.can_be_required.push_back(id);
    }
  }

  std::sort(categories.can_be_required.begin(), categories.can_be_required.end());
  std::sort(categories.cannot_be_required.begin(), categories.cannot_be_required.end());
  return categories;
}

const BuiltinIndex& GetBuiltinIndex() {
  static const BuiltinIndex index = []() {
    BuiltinIndex out;
    out.all_ids.reserve(generated::kBuiltinEntries.size());
    out.by_id.reserve(generated::kBuiltinEntries.size());
    out.by_path.reserve(generated::kBuiltinEntries.size());

    for (const generated::BuiltinEntry& entry : generated::kBuiltinEntries) {
      out.all_ids.emplace_back(entry.id);
      out.by_id.emplace(out.all_ids.back(), &entry);

      const fs::path root = entry.is_internal_dep ? NodeDepsRoot() : NodeLibRoot();
      const fs::path full_path = root / fs::path(std::string(entry.relative_path));
      out.by_path.emplace(NormalizePathKey(full_path), &entry);
    }

    out.categories = BuildBuiltinCategories(out.all_ids);
    return out;
  }();
  return index;
}

bool NormalizeBuiltinSpecifier(const std::string& specifier, std::string* out) {
  if (out == nullptr) return false;

  std::string id = specifier;
  if (id.starts_with(kNodePrefix)) {
    id.erase(0, kNodePrefix.size());
  }
  if (id.empty() || id.front() == '.') return false;

  const auto& index = GetBuiltinIndex();
  if (index.by_id.find(id) == index.by_id.end()) return false;
  *out = std::move(id);
  return true;
}

fs::path EntryPath(const generated::BuiltinEntry& entry) {
  const fs::path root = entry.is_internal_dep ? NodeDepsRoot() : NodeLibRoot();
  return root / fs::path(std::string(entry.relative_path));
}

const generated::BuiltinEntry* LookupBuiltinByPath(const fs::path& resolved_path) {
  const auto& index = GetBuiltinIndex();
  const auto it = index.by_path.find(NormalizePathKey(resolved_path));
  if (it == index.by_path.end()) return nullptr;
  return it->second;
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

  std::string id;
  if (!NormalizeBuiltinSpecifier(specifier, &id)) return false;

  const auto& index = GetBuiltinIndex();
  const auto it = index.by_id.find(id);
  if (it == index.by_id.end()) return false;

  const generated::BuiltinEntry* entry = it->second;
  const fs::path root = entry->is_internal_dep ? NodeDepsRoot() : NodeLibRoot();
  *out_path = fs::absolute(root / fs::path(std::string(entry->relative_path))).lexically_normal();
  return true;
}

bool TryGetBuiltinIdForPath(const fs::path& resolved_path, std::string* out_id) {
  if (out_id == nullptr) return false;

  const generated::BuiltinEntry* entry = LookupBuiltinByPath(resolved_path);
  if (entry == nullptr) return false;

  *out_id = std::string(entry->id);
  return true;
}

bool TryReadBuiltinSource(const fs::path& resolved_path, std::string* out_source) {
  if (out_source == nullptr) return false;

  const generated::BuiltinEntry* entry = LookupBuiltinByPath(resolved_path);
  if (entry == nullptr) return false;

  return ReadEntrySource(*entry, out_source, nullptr) == SourceReadStatus::kOk;
}

bool TryReadBuiltinSource(const std::string& specifier, std::string* out_source) {
  return ReadBuiltinSource(specifier, out_source, nullptr) == SourceReadStatus::kOk;
}

SourceReadStatus ReadBuiltinSource(const std::string& specifier,
                                   std::string* out_source,
                                   std::string* error_out) {
  if (out_source == nullptr) return SourceReadStatus::kNotBuiltin;

  std::string id;
  if (!NormalizeBuiltinSpecifier(specifier, &id)) return SourceReadStatus::kNotBuiltin;

  const auto& index = GetBuiltinIndex();
  const auto it = index.by_id.find(id);
  if (it == index.by_id.end()) return SourceReadStatus::kNotBuiltin;

  return ReadEntrySource(*it->second, out_source, error_out);
}

const std::vector<std::string>& AllBuiltinIds() {
  return GetBuiltinIndex().all_ids;
}

const BuiltinCategories& GetBuiltinCategories() {
  return GetBuiltinIndex().categories;
}

bool BuiltinSourcesEmbedded() {
  return generated::kBuiltinSourcesEmbedded;
}

namespace {

// Files the preflight requires before the first bootstrapper runs: the two
// bootstrap entry points plus one file from each deps subtree the runtime
// loads early, so a partially staged tree fails here rather than mid-run.
constexpr const char* kPreflightSentinelIds[] = {
    "internal/per_context/primordials",
    "internal/bootstrap/realm",
    "internal/deps/cjs-module-lexer/lexer",
    "internal/deps/undici/undici",
};

// Written by scripts/stage_wasix_static_data.py next to the staged tree.
constexpr const char kCatalogStampName[] = ".edge-builtin-catalog";

struct PreflightResult {
  std::string error;    // Non-empty: bootstrap must not start.
  std::string warning;  // Non-empty: printed once, bootstrap continues.
};

std::string TrimTrailingWhitespace(std::string text) {
  while (!text.empty() &&
         (text.back() == '\n' || text.back() == '\r' || text.back() == ' ' || text.back() == '\t')) {
    text.pop_back();
  }
  return text;
}

PreflightResult RunFileModePreflight() {
  PreflightResult result;
  const char* explicit_root = ExplicitLibRoot();
  const fs::path& root = NodeLibRoot();
  const std::string remedy =
      explicit_root != nullptr
          ? std::string(kNodeLibRootEnvVar) + "=" + explicit_root +
                " must point at a node-lib tree staged by scripts/stage_wasix_static_data.py "
                "for this build"
          : std::string("this build reads builtin modules from the filesystem: mount the "
                        "package's /node-lib directory (wasmer.toml [fs] \"/node-lib\") or set ") +
                kNodeLibRootEnvVar + " to a staged node-lib tree";

  std::error_code ec;
  if (!fs::is_directory(root, ec)) {
    result.error = "Edge builtin JS not found at " + root.string() + " (not a directory); " + remedy;
    return result;
  }

  const auto& index = GetBuiltinIndex();
  for (const char* id : kPreflightSentinelIds) {
    const auto it = index.by_id.find(id);
    if (it == index.by_id.end()) continue;  // Not part of this catalog.
    const fs::path path = EntryPath(*it->second);
    if (fs::is_regular_file(path, ec)) continue;
    result.error = "Edge builtin JS not found at " + root.string() + ": missing " +
                   path.string() + "; " + remedy;
    return result;
  }

  const fs::path stamp_path = root / kCatalogStampName;
  std::string stamp;
  int read_errno = 0;
  std::string problem;
  if (!ReadWholeFile(stamp_path, &stamp, &read_errno)) {
    problem = "cannot read " + stamp_path.string() + " (" + DescribeErrno(read_errno) + ")";
  } else {
    stamp = TrimTrailingWhitespace(std::move(stamp));
    if (stamp != generated::kBuiltinCatalogDigest) {
      problem = stamp_path.string() + " records catalog " + stamp + ", but this binary expects " +
                std::string(generated::kBuiltinCatalogDigest);
    }
  }
  if (problem.empty()) return result;

  if (explicit_root != nullptr) {
    result.warning = "builtin JS at " + root.string() + " may not match this binary: " + problem +
                     ". Builtin modules can misbehave if " + kNodeLibRootEnvVar +
                     " points at a tree staged from a different build.";
  } else {
    result.error = "Edge builtin JS at " + root.string() + " does not match this binary: " +
                   problem + "; the node-lib tree must be staged from the same build as the "
                   "binary (scripts/stage_wasix_static_data.py)";
  }
  return result;
}

}  // namespace

bool VerifyBuiltinSources(std::string* error_out) {
  if constexpr (generated::kBuiltinSourcesEmbedded) {
    return true;
  }
  // The lib root is fixed for the process, so check it once; worker
  // bootstraps reuse the result and the warning prints a single time.
  static const PreflightResult result = RunFileModePreflight();
  static std::once_flag warning_once;
  if (!result.warning.empty()) {
    std::call_once(warning_once, []() {
      std::fprintf(stderr, "[edge] warning: %s\n", result.warning.c_str());
    });
  }
  if (!result.error.empty()) {
    if (error_out != nullptr) *error_out = result.error;
    return false;
  }
  return true;
}

}  // namespace builtin_catalog
