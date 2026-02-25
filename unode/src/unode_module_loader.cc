#include "unode_module_loader.h"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

namespace fs = std::filesystem;

struct ModuleLoaderState {
  std::unordered_map<std::string, napi_ref> module_cache;
  napi_ref cache_object_ref = nullptr;
  std::string entry_dir;
};

struct RequireContext {
  ModuleLoaderState* state;
  std::string base_dir;
};

std::unordered_map<napi_env, ModuleLoaderState> g_loader_states;
std::vector<RequireContext*> g_require_contexts;

std::string ReadTextFile(const fs::path& path) {
  std::ifstream in(path);
  if (!in.is_open()) {
    return "";
  }
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

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

bool PathExistsRegularFile(const fs::path& path) {
  std::error_code ec;
  return fs::exists(path, ec) && fs::is_regular_file(path, ec);
}

bool PathExistsDirectory(const fs::path& path) {
  std::error_code ec;
  return fs::exists(path, ec) && fs::is_directory(path, ec);
}

bool ResolveAsFile(const fs::path& candidate, fs::path* out) {
  if (PathExistsRegularFile(candidate)) {
    *out = candidate;
    return true;
  }
  if (candidate.has_extension()) {
    return false;
  }

  const fs::path js_path = candidate.string() + ".js";
  if (PathExistsRegularFile(js_path)) {
    *out = js_path;
    return true;
  }
  const fs::path json_path = candidate.string() + ".json";
  if (PathExistsRegularFile(json_path)) {
    *out = json_path;
    return true;
  }
  return false;
}

bool ParsePackageMain(const fs::path& package_json_path, std::string* main_out) {
  const std::string source = ReadTextFile(package_json_path);
  if (source.empty()) {
    return false;
  }

  const std::string needle = "\"main\"";
  const size_t key_pos = source.find(needle);
  if (key_pos == std::string::npos) {
    return false;
  }
  const size_t colon_pos = source.find(':', key_pos + needle.size());
  if (colon_pos == std::string::npos) {
    return false;
  }
  const size_t first_quote = source.find('"', colon_pos + 1);
  if (first_quote == std::string::npos) {
    return false;
  }
  const size_t second_quote = source.find('"', first_quote + 1);
  if (second_quote == std::string::npos || second_quote <= first_quote + 1) {
    return false;
  }
  *main_out = source.substr(first_quote + 1, second_quote - first_quote - 1);
  return true;
}

bool ResolveAsDirectory(const fs::path& candidate, fs::path* out) {
  if (!PathExistsDirectory(candidate)) {
    return false;
  }

  const fs::path package_json = candidate / "package.json";
  if (PathExistsRegularFile(package_json)) {
    std::string main_entry;
    if (ParsePackageMain(package_json, &main_entry) && !main_entry.empty()) {
      fs::path resolved_main;
      const fs::path main_candidate = candidate / main_entry;
      if (ResolveAsFile(main_candidate, &resolved_main) || ResolveAsDirectory(main_candidate, &resolved_main)) {
        *out = resolved_main;
        return true;
      }
    }
  }

  const fs::path index_js = candidate / "index.js";
  if (PathExistsRegularFile(index_js)) {
    *out = index_js;
    return true;
  }
  const fs::path index_json = candidate / "index.json";
  if (PathExistsRegularFile(index_json)) {
    *out = index_json;
    return true;
  }
  return false;
}

bool ResolveModulePath(const std::string& specifier, const std::string& base_dir, fs::path* out) {
  fs::path raw_path;
  if (!specifier.empty() && specifier[0] == '/') {
    raw_path = fs::path(specifier);
  } else if (specifier.rfind("./", 0) == 0 || specifier.rfind("../", 0) == 0 || specifier == "." ||
             specifier == "..") {
    raw_path = fs::path(base_dir) / specifier;
  } else {
    return false;
  }

  const fs::path normalized = fs::absolute(raw_path).lexically_normal();
  fs::path resolved;
  if (ResolveAsFile(normalized, &resolved) || ResolveAsDirectory(normalized, &resolved)) {
    *out = resolved.lexically_normal();
    return true;
  }
  return false;
}

napi_value MakeError(napi_env env, const char* code, const std::string& message) {
  napi_value code_value = nullptr;
  napi_value msg_value = nullptr;
  napi_value error_value = nullptr;
  if (napi_create_string_utf8(env, code, NAPI_AUTO_LENGTH, &code_value) != napi_ok ||
      napi_create_string_utf8(env, message.c_str(), NAPI_AUTO_LENGTH, &msg_value) != napi_ok ||
      napi_create_error(env, code_value, msg_value, &error_value) != napi_ok) {
    return nullptr;
  }
  if (napi_set_named_property(env, error_value, "code", code_value) != napi_ok) {
    return nullptr;
  }
  return error_value;
}

bool ThrowModuleNotFound(napi_env env, const std::string& specifier) {
  const std::string message = "Cannot find module '" + specifier + "'";
  napi_value error_value = MakeError(env, "MODULE_NOT_FOUND", message);
  if (error_value == nullptr) {
    return false;
  }
  return napi_throw(env, error_value) == napi_ok;
}

bool ThrowLoaderError(napi_env env, const std::string& message) {
  napi_value error_value = MakeError(env, "ERR_UNODE_MODULE_LOAD", message);
  if (error_value == nullptr) {
    return false;
  }
  return napi_throw(env, error_value) == napi_ok;
}

napi_value GetGlobal(napi_env env) {
  napi_value global = nullptr;
  if (napi_get_global(env, &global) != napi_ok || global == nullptr) {
    return nullptr;
  }
  return global;
}

napi_value GetCacheObject(napi_env env, ModuleLoaderState* state) {
  if (state == nullptr) {
    return nullptr;
  }
  if (state->cache_object_ref == nullptr) {
    napi_value cache_obj = nullptr;
    if (napi_create_object(env, &cache_obj) != napi_ok || cache_obj == nullptr) {
      return nullptr;
    }
    if (napi_create_reference(env, cache_obj, 1, &state->cache_object_ref) != napi_ok || state->cache_object_ref == nullptr) {
      return nullptr;
    }
    return cache_obj;
  }
  napi_value cache_obj = nullptr;
  if (napi_get_reference_value(env, state->cache_object_ref, &cache_obj) != napi_ok || cache_obj == nullptr) {
    return nullptr;
  }
  return cache_obj;
}

napi_value GetCachedExportsFromJsCache(napi_env env, ModuleLoaderState* state, const std::string& resolved_key) {
  napi_value cache_obj = GetCacheObject(env, state);
  if (cache_obj == nullptr) {
    return nullptr;
  }
  bool has_entry = false;
  if (napi_has_named_property(env, cache_obj, resolved_key.c_str(), &has_entry) != napi_ok || !has_entry) {
    return nullptr;
  }
  napi_value cache_entry = nullptr;
  if (napi_get_named_property(env, cache_obj, resolved_key.c_str(), &cache_entry) != napi_ok || cache_entry == nullptr) {
    return nullptr;
  }
  bool has_exports = false;
  if (napi_has_named_property(env, cache_entry, "exports", &has_exports) != napi_ok || !has_exports) {
    return nullptr;
  }
  napi_value exports_value = nullptr;
  if (napi_get_named_property(env, cache_entry, "exports", &exports_value) != napi_ok || exports_value == nullptr) {
    return nullptr;
  }
  return exports_value;
}

napi_value CreateResolvedPathString(napi_env env, const fs::path& resolved_path) {
  napi_value out = nullptr;
  if (napi_create_string_utf8(env, resolved_path.string().c_str(), NAPI_AUTO_LENGTH, &out) != napi_ok || out == nullptr) {
    return nullptr;
  }
  return out;
}

napi_value ResolveSpecifierForContext(napi_env env, RequireContext* context, const std::string& specifier, bool throw_on_error) {
  fs::path resolved_path;
  if (!ResolveModulePath(specifier, context->base_dir, &resolved_path)) {
    if (throw_on_error) {
      ThrowModuleNotFound(env, specifier);
    }
    return nullptr;
  }
  return CreateResolvedPathString(env, resolved_path);
}

napi_value RequireResolveCallback(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  void* data = nullptr;
  if (napi_get_cb_info(env, info, &argc, argv, nullptr, &data) != napi_ok || data == nullptr) {
    return nullptr;
  }
  auto* context = static_cast<RequireContext*>(data);
  if (context->state == nullptr) {
    ThrowLoaderError(env, "Invalid require.resolve context");
    return nullptr;
  }
  if (argc < 1 || argv[0] == nullptr) {
    ThrowLoaderError(env, "Missing module specifier");
    return nullptr;
  }
  const std::string specifier = ValueToUtf8(env, argv[0]);
  if (specifier.empty()) {
    ThrowLoaderError(env, "Empty module specifier");
    return nullptr;
  }

  napi_value resolved_path = ResolveSpecifierForContext(env, context, specifier, false);
  if (resolved_path != nullptr) {
    return resolved_path;
  }

  // Node allows cached bare specifiers to resolve to themselves.
  if (GetCachedExportsFromJsCache(env, context->state, specifier) != nullptr) {
    napi_value key_value = nullptr;
    if (napi_create_string_utf8(env, specifier.c_str(), NAPI_AUTO_LENGTH, &key_value) == napi_ok && key_value != nullptr) {
      return key_value;
    }
    return nullptr;
  }

  ThrowModuleNotFound(env, specifier);
  return nullptr;
}

napi_value CreateRequireFunction(napi_env env, RequireContext* context);

bool EvaluateJsModule(napi_env env,
                      const fs::path& resolved_path,
                      napi_value module_obj,
                      napi_value exports_obj,
                      napi_value require_fn) {
  const std::string source = ReadTextFile(resolved_path);
  if (source.empty()) {
    return ThrowLoaderError(env, "Failed to read module source");
  }

  const std::string wrapped_source =
      "(function(exports, require, module, __filename, __dirname) {\n" + source + "\n})";
  napi_value script_source = nullptr;
  if (napi_create_string_utf8(env, wrapped_source.c_str(), NAPI_AUTO_LENGTH, &script_source) != napi_ok ||
      script_source == nullptr) {
    return ThrowLoaderError(env, "Failed to create wrapped module source");
  }

  napi_value wrapped_fn = nullptr;
  if (napi_run_script(env, script_source, &wrapped_fn) != napi_ok || wrapped_fn == nullptr) {
    return false;  // Preserve JS exception.
  }

  napi_value filename_value = nullptr;
  napi_value dirname_value = nullptr;
  if (napi_create_string_utf8(env, resolved_path.string().c_str(), NAPI_AUTO_LENGTH, &filename_value) != napi_ok ||
      napi_create_string_utf8(env, resolved_path.parent_path().string().c_str(), NAPI_AUTO_LENGTH, &dirname_value) !=
          napi_ok) {
    return ThrowLoaderError(env, "Failed to build module path values");
  }

  napi_value argv[5] = {exports_obj, require_fn, module_obj, filename_value, dirname_value};
  napi_value global = GetGlobal(env);
  if (global == nullptr) {
    return ThrowLoaderError(env, "Failed to fetch global object");
  }
  napi_value call_result = nullptr;
  if (napi_call_function(env, global, wrapped_fn, 5, argv, &call_result) != napi_ok) {
    return false;  // Preserve JS exception.
  }
  return true;
}

bool ParseJsonModule(napi_env env, const fs::path& resolved_path, napi_value module_obj) {
  const std::string source = ReadTextFile(resolved_path);
  if (source.empty()) {
    return ThrowLoaderError(env, "Failed to read JSON module");
  }

  const std::string parse_source = "(function(__text){ return JSON.parse(__text); })";
  napi_value parse_script = nullptr;
  if (napi_create_string_utf8(env, parse_source.c_str(), NAPI_AUTO_LENGTH, &parse_script) != napi_ok ||
      parse_script == nullptr) {
    return ThrowLoaderError(env, "Failed to prepare JSON parser");
  }
  napi_value parse_fn = nullptr;
  if (napi_run_script(env, parse_script, &parse_fn) != napi_ok || parse_fn == nullptr) {
    return false;
  }

  napi_value json_text = nullptr;
  if (napi_create_string_utf8(env, source.c_str(), NAPI_AUTO_LENGTH, &json_text) != napi_ok || json_text == nullptr) {
    return ThrowLoaderError(env, "Failed to create JSON source string");
  }

  napi_value global = GetGlobal(env);
  if (global == nullptr) {
    return ThrowLoaderError(env, "Failed to fetch global object");
  }
  napi_value parsed = nullptr;
  if (napi_call_function(env, global, parse_fn, 1, &json_text, &parsed) != napi_ok || parsed == nullptr) {
    return false;  // Preserve syntax exception from JSON.parse.
  }

  return napi_set_named_property(env, module_obj, "exports", parsed) == napi_ok;
}

bool LoadResolvedModule(napi_env env, ModuleLoaderState* state, const fs::path& resolved_path, napi_value* out_exports);

napi_value RequireCallback(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  void* data = nullptr;
  if (napi_get_cb_info(env, info, &argc, argv, nullptr, &data) != napi_ok || data == nullptr) {
    return nullptr;
  }
  auto* context = static_cast<RequireContext*>(data);
  if (context->state == nullptr) {
    ThrowLoaderError(env, "Invalid require context");
    return nullptr;
  }

  if (argc < 1 || argv[0] == nullptr) {
    ThrowLoaderError(env, "Missing module specifier");
    return nullptr;
  }
  const std::string specifier = ValueToUtf8(env, argv[0]);
  if (specifier.empty()) {
    ThrowLoaderError(env, "Empty module specifier");
    return nullptr;
  }

  napi_value from_js_cache = GetCachedExportsFromJsCache(env, context->state, specifier);
  if (from_js_cache != nullptr) {
    return from_js_cache;
  }

  napi_value resolved_path_value = ResolveSpecifierForContext(env, context, specifier, true);
  if (resolved_path_value == nullptr) {
    return nullptr;
  }
  const std::string resolved_key = ValueToUtf8(env, resolved_path_value);
  if (resolved_key.empty()) {
    ThrowLoaderError(env, "Failed to resolve module path");
    return nullptr;
  }

  fs::path resolved_path = fs::path(resolved_key);
  from_js_cache = GetCachedExportsFromJsCache(env, context->state, resolved_key);
  if (from_js_cache != nullptr) {
    return from_js_cache;
  }

  napi_value exports_value = nullptr;
  if (!LoadResolvedModule(env, context->state, resolved_path, &exports_value) || exports_value == nullptr) {
    return nullptr;
  }
  return exports_value;
}

napi_value CreateRequireFunction(napi_env env, RequireContext* context) {
  napi_value require_fn = nullptr;
  if (napi_create_function(env, "require", NAPI_AUTO_LENGTH, RequireCallback, context, &require_fn) != napi_ok ||
      require_fn == nullptr) {
    return nullptr;
  }
  napi_value cache_obj = GetCacheObject(env, context->state);
  if (cache_obj == nullptr) {
    return nullptr;
  }
  if (napi_set_named_property(env, require_fn, "cache", cache_obj) != napi_ok) {
    return nullptr;
  }
  napi_value resolve_fn = nullptr;
  if (napi_create_function(env, "resolve", NAPI_AUTO_LENGTH, RequireResolveCallback, context, &resolve_fn) != napi_ok ||
      resolve_fn == nullptr) {
    return nullptr;
  }
  if (napi_set_named_property(env, require_fn, "resolve", resolve_fn) != napi_ok) {
    return nullptr;
  }
  return require_fn;
}

bool GetCachedModuleExports(napi_env env, ModuleLoaderState* state, const std::string& resolved_key, napi_value* out) {
  auto it = state->module_cache.find(resolved_key);
  if (it == state->module_cache.end()) {
    return false;
  }
  napi_value module_obj = nullptr;
  if (napi_get_reference_value(env, it->second, &module_obj) != napi_ok || module_obj == nullptr) {
    return false;
  }
  return napi_get_named_property(env, module_obj, "exports", out) == napi_ok && *out != nullptr;
}

bool CacheModule(napi_env env, ModuleLoaderState* state, const std::string& resolved_key, napi_value module_obj) {
  napi_ref ref = nullptr;
  if (napi_create_reference(env, module_obj, 1, &ref) != napi_ok || ref == nullptr) {
    return false;
  }
  auto existing = state->module_cache.find(resolved_key);
  if (existing != state->module_cache.end()) {
    napi_delete_reference(env, existing->second);
  }
  state->module_cache[resolved_key] = ref;
  napi_value cache_obj = GetCacheObject(env, state);
  if (cache_obj == nullptr) {
    return false;
  }
  if (napi_set_named_property(env, cache_obj, resolved_key.c_str(), module_obj) != napi_ok) {
    return false;
  }
  return true;
}

void RemoveCachedModule(napi_env env, ModuleLoaderState* state, const std::string& resolved_key) {
  auto it = state->module_cache.find(resolved_key);
  if (it == state->module_cache.end()) {
    return;
  }
  napi_delete_reference(env, it->second);
  state->module_cache.erase(it);
  napi_value cache_obj = GetCacheObject(env, state);
  if (cache_obj != nullptr) {
    napi_value cache_key = nullptr;
    if (napi_create_string_utf8(env, resolved_key.c_str(), NAPI_AUTO_LENGTH, &cache_key) == napi_ok &&
        cache_key != nullptr) {
      bool ignored = false;
      napi_delete_property(env, cache_obj, cache_key, &ignored);
    }
  }
}

bool LoadResolvedModule(napi_env env, ModuleLoaderState* state, const fs::path& resolved_path, napi_value* out_exports) {
  const std::string resolved_key = resolved_path.string();
  if (GetCachedModuleExports(env, state, resolved_key, out_exports)) {
    return true;
  }

  napi_value module_obj = nullptr;
  if (napi_create_object(env, &module_obj) != napi_ok || module_obj == nullptr) {
    ThrowLoaderError(env, "Failed to create module object");
    return false;
  }
  napi_value exports_obj = nullptr;
  if (napi_create_object(env, &exports_obj) != napi_ok || exports_obj == nullptr) {
    ThrowLoaderError(env, "Failed to create exports object");
    return false;
  }
  napi_value filename_value = nullptr;
  if (napi_create_string_utf8(env, resolved_key.c_str(), NAPI_AUTO_LENGTH, &filename_value) != napi_ok ||
      filename_value == nullptr) {
    ThrowLoaderError(env, "Failed to create module filename");
    return false;
  }
  if (napi_set_named_property(env, module_obj, "id", filename_value) != napi_ok ||
      napi_set_named_property(env, module_obj, "filename", filename_value) != napi_ok ||
      napi_set_named_property(env, module_obj, "exports", exports_obj) != napi_ok) {
    ThrowLoaderError(env, "Failed to initialize module object");
    return false;
  }
  if (!CacheModule(env, state, resolved_key, module_obj)) {
    ThrowLoaderError(env, "Failed to cache module");
    return false;
  }

  auto* child_context = new RequireContext{state, resolved_path.parent_path().string()};
  g_require_contexts.push_back(child_context);
  napi_value require_fn = CreateRequireFunction(env, child_context);
  if (require_fn == nullptr) {
    RemoveCachedModule(env, state, resolved_key);
    ThrowLoaderError(env, "Failed to create require function");
    return false;
  }

  const std::string ext = resolved_path.extension().string();
  bool ok = false;
  if (ext == ".json") {
    ok = ParseJsonModule(env, resolved_path, module_obj);
  } else {
    ok = EvaluateJsModule(env, resolved_path, module_obj, exports_obj, require_fn);
  }
  if (!ok) {
    RemoveCachedModule(env, state, resolved_key);
    return false;
  }

  napi_value updated_exports = nullptr;
  if (napi_get_named_property(env, module_obj, "exports", &updated_exports) != napi_ok || updated_exports == nullptr) {
    RemoveCachedModule(env, state, resolved_key);
    ThrowLoaderError(env, "Failed to fetch module exports");
    return false;
  }
  *out_exports = updated_exports;
  return true;
}

}  // namespace

napi_status UnodeInstallModuleLoader(napi_env env, const char* entry_script_path) {
  if (env == nullptr) {
    return napi_invalid_arg;
  }

  fs::path entry_path;
  if (entry_script_path != nullptr && entry_script_path[0] != '\0') {
    entry_path = fs::absolute(fs::path(entry_script_path)).lexically_normal();
  } else {
    entry_path = fs::current_path() / "<eval>";
  }

  auto& state = g_loader_states[env];
  state.entry_dir = entry_path.parent_path().string();

  auto* root_context = new RequireContext{&state, state.entry_dir};
  g_require_contexts.push_back(root_context);

  napi_value require_fn = CreateRequireFunction(env, root_context);
  if (require_fn == nullptr) {
    return napi_generic_failure;
  }
  napi_value cache_obj = GetCacheObject(env, &state);
  if (cache_obj == nullptr) {
    return napi_generic_failure;
  }

  napi_value global = nullptr;
  if (napi_get_global(env, &global) != napi_ok || global == nullptr) {
    return napi_generic_failure;
  }

  napi_value filename_value = nullptr;
  napi_value dirname_value = nullptr;
  if (napi_create_string_utf8(env, entry_path.string().c_str(), NAPI_AUTO_LENGTH, &filename_value) != napi_ok ||
      napi_create_string_utf8(env, state.entry_dir.c_str(), NAPI_AUTO_LENGTH, &dirname_value) != napi_ok) {
    return napi_generic_failure;
  }

  if (napi_set_named_property(env, global, "require", require_fn) != napi_ok ||
      napi_set_named_property(env, require_fn, "cache", cache_obj) != napi_ok ||
      napi_set_named_property(env, global, "__filename", filename_value) != napi_ok ||
      napi_set_named_property(env, global, "__dirname", dirname_value) != napi_ok) {
    return napi_generic_failure;
  }
  return napi_ok;
}
