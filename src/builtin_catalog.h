#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace builtin_catalog {

const std::filesystem::path& NodeLibRoot();
const std::filesystem::path& NodeDepsRoot();

bool ResolveBuiltinId(const std::string& specifier, std::filesystem::path* out_path);
bool TryGetBuiltinIdForPath(const std::filesystem::path& resolved_path, std::string* out_id);
bool TryReadBuiltinSource(const std::filesystem::path& resolved_path, std::string* out_source);
bool TryReadBuiltinSource(const std::string& specifier, std::string* out_source);

const std::vector<std::string>& AllBuiltinIds();

}  // namespace builtin_catalog
