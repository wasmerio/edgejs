#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace builtin_catalog {

struct BuiltinCategories {
  std::vector<std::string> can_be_required;
  std::vector<std::string> cannot_be_required;
};

const std::filesystem::path& NodeLibRoot();
const std::filesystem::path& NodeDepsRoot();

bool ResolveBuiltinId(const std::string& specifier, std::filesystem::path* out_path);
bool TryGetBuiltinIdForPath(const std::filesystem::path& resolved_path, std::string* out_id);
bool TryReadBuiltinSource(const std::filesystem::path& resolved_path, std::string* out_source);
bool TryReadBuiltinSource(const std::string& specifier, std::string* out_source);

enum class SourceReadStatus {
  kOk,
  kNotBuiltin,  // The specifier is not a builtin id.
  kIoError,     // A builtin whose file could not be read (file-backed builds).
};

// Like TryReadBuiltinSource, but tells an unknown id apart from a known
// builtin whose source file could not be read. For kIoError, *error_out (when
// non-null) names the module, the path, and the errno.
SourceReadStatus ReadBuiltinSource(const std::string& specifier,
                                   std::string* out_source,
                                   std::string* error_out);

const std::vector<std::string>& AllBuiltinIds();
const BuiltinCategories& GetBuiltinCategories();

// True when builtin JS sources are compiled into the binary. WASIX builds
// leave them out (EDGE_EMBED_BUILTIN_SOURCES=OFF) and read them from the lib
// root: EDGEJS_LIB_DIR when set, otherwise the package's /node-lib mount, with
// deps under <lib root>/internal/deps.
bool BuiltinSourcesEmbedded();

// Preflight for builds that read builtin sources from the filesystem; a no-op
// returning true when sources are embedded. Checks, once per process, that the
// lib root holds the bootstrap and deps sentinel files and that its
// .edge-builtin-catalog stamp matches this binary's catalog digest, so a
// missing or stale /node-lib tree fails with an actionable message instead of
// deep inside the first bootstrapper. A missing or mismatched stamp is an
// error for the default /node-lib root and a one-time stderr warning when
// EDGEJS_LIB_DIR selects the root. Returns false and sets *error_out on error.
bool VerifyBuiltinSources(std::string* error_out);

}  // namespace builtin_catalog
