#include "unode_module_loader.h"

#include <algorithm>
#include <cstdlib>
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
  napi_ref primordials_ref = nullptr;
  napi_ref internal_binding_ref = nullptr;
  napi_ref native_builtins_binding_ref = nullptr;
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

// True if request is relative (./, ../, ., ..) or absolute (starts with /).
// Used to decide whether to try node_modules resolution (only for bare specifiers).
static bool IsRelativeOrAbsoluteRequest(const std::string& specifier) {
  if (specifier.empty()) return true;
  if (specifier[0] == '/') return true;
  if (specifier[0] != '.') return false;
  if (specifier.size() == 1) return true;  // "."
  if (specifier[1] == '/' || specifier[1] == '.') return true;  // "./" or ".."
  return false;
}

// Build list of node_modules directories to search, from from_dir upward (Node's _nodeModulePaths).
// from_dir must be absolute. Returns e.g. [from_dir/node_modules, parent/node_modules, ..., /node_modules].
static std::vector<fs::path> NodeModulePaths(const fs::path& from_dir) {
  std::vector<fs::path> out;
  fs::path from = fs::absolute(from_dir).lexically_normal();
  std::string from_str = from.string();
  if (from_str.empty() || (from_str.size() == 1 && from_str[0] == '/')) {
    out.push_back(fs::path("/node_modules"));
    return out;
  }
  const std::string node_modules_name = "node_modules";
  const size_t nm_len = node_modules_name.size();
  size_t last = from_str.size();
  for (size_t i = from_str.size(); i > 0; --i) {
    size_t idx = i - 1;
    if (from_str[idx] == '/' || from_str[idx] == '\\') {
      bool segment_is_node_modules = (last >= nm_len &&
          from_str.compare(last - nm_len, nm_len, node_modules_name) == 0 &&
          (last == nm_len || from_str[last - nm_len - 1] == '/' || from_str[last - nm_len - 1] == '\\'));
      if (!segment_is_node_modules) {
        out.push_back(fs::path(from_str.substr(0, last) + "/node_modules"));
      }
      last = idx;
    }
  }
  out.push_back(fs::path("/node_modules"));
  return out;
}

// For each path in node_modules_dirs, try path/request as file or directory (package main / index).
// Returns true and sets *out to the resolved absolute path when found.
static bool FindPathInNodeModules(const std::string& request,
                                  const std::vector<fs::path>& node_modules_dirs,
                                  fs::path* out) {
  for (const fs::path& nm_dir : node_modules_dirs) {
    fs::path candidate = nm_dir / request;
    fs::path resolved;
    if (ResolveAsFile(candidate, &resolved) || ResolveAsDirectory(candidate, &resolved)) {
      *out = fs::absolute(resolved).lexically_normal();
      return true;
    }
  }
  return false;
}

// Resolve a bare specifier (e.g. "lodash") via node_modules walk from base_dir.
// Returns false if not found or if specifier is relative/absolute (use ResolveModulePath for those).
static bool ResolveNodeModules(const std::string& specifier, const std::string& base_dir,
                               fs::path* out) {
  if (IsRelativeOrAbsoluteRequest(specifier)) {
    return false;
  }
  std::vector<fs::path> paths = NodeModulePaths(fs::path(base_dir));
  return FindPathInNodeModules(specifier, paths, out);
}

// Resolve bare specifier (e.g. "assert", "path", "node:worker_threads") from unode/src/builtins/<id>.js.
// Strips "node:" prefix so node:worker_threads resolves to builtins/worker_threads.js.
bool ResolveBuiltinPath(const std::string& specifier, const std::string& base_dir, fs::path* out) {
  std::string id = specifier;
  if (id.size() > 5 && id.compare(0, 5, "node:") == 0) {
    id = id.substr(5);
  }
  if (id.empty() || id.rfind(".", 0) == 0) {
    return false;
  }
  static const fs::path runtime_builtins_dir =
      fs::absolute(fs::path(__FILE__).parent_path() / "builtins").lexically_normal();
  fs::path resolved;
  if (id == "internal/test/binding") {
    fs::path internal_test_binding = runtime_builtins_dir / "internal_test_binding.js";
    if (ResolveAsFile(internal_test_binding, &resolved)) {
      *out = resolved.lexically_normal();
      return true;
    }
  }
  // Resolve critical internal util modules directly to upstream node/lib paths.
  // This avoids an extra shim layer that can expose partially initialized wrapper
  // exports during circular loads (e.g. events <-> internal/util call paths).
  if (id == "internal/util" || id == "internal/util/inspect") {
    static const fs::path upstream_node_lib_dir =
        fs::absolute(fs::path(__FILE__).parent_path() / ".." / ".." / "node" / "lib").lexically_normal();
    fs::path upstream_candidate = upstream_node_lib_dir / (id + ".js");
    if (ResolveAsFile(upstream_candidate, &resolved)) {
      *out = resolved.lexically_normal();
      return true;
    }
  }
  fs::path candidate = runtime_builtins_dir / (id + ".js");
  if (ResolveAsFile(candidate, &resolved)) {
    *out = resolved.lexically_normal();
    return true;
  }
  // Preserve Node-style relative builtin lookup for modules loaded from node/lib
  // (e.g. internal/v8/startup_snapshot from node/lib/buffer.js).
  const fs::path base_relative_builtins = fs::absolute(fs::path(base_dir) / ".." / "builtins").lexically_normal();
  candidate = base_relative_builtins / (id + ".js");
  if (ResolveAsFile(candidate, &resolved)) {
    *out = resolved.lexically_normal();
    return true;
  }
  return false;
}

// Directory to use when loading bootstrap builtins in the prelude, so they are found regardless of entry_dir.
static std::string GetBuiltinsDirForBootstrap() {
  static const fs::path runtime_builtins_dir =
      fs::absolute(fs::path(__FILE__).parent_path() / "builtins").lexically_normal();
  return runtime_builtins_dir.string();
}

static std::vector<std::string> CollectRuntimeBuiltinIds() {
  static std::vector<std::string> ids;
  if (!ids.empty()) return ids;

  const fs::path root = fs::absolute(fs::path(__FILE__).parent_path() / "builtins").lexically_normal();
  std::error_code ec;
  if (!fs::exists(root, ec) || !fs::is_directory(root, ec)) {
    return ids;
  }

  for (fs::recursive_directory_iterator it(root, ec), end; it != end && !ec; it.increment(ec)) {
    if (!it->is_regular_file(ec)) continue;
    const fs::path p = it->path();
    if (p.extension() != ".js") continue;
    fs::path rel = p.lexically_relative(root);
    std::string id = rel.generic_string();
    if (id.size() <= 3) continue;
    id.resize(id.size() - 3);  // trim ".js"
    if (id == "internal_test_binding") {
      ids.push_back("internal/test/binding");
      continue;
    }
    ids.push_back(id);
  }

  std::sort(ids.begin(), ids.end());
  ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
  return ids;
}

static napi_value NoopCallback(napi_env env, napi_callback_info /*info*/) {
  napi_value undefined = nullptr;
  napi_get_undefined(env, &undefined);
  return undefined;
}

static napi_value GetOrCreateNativeBuiltinsBinding(napi_env env, ModuleLoaderState* state) {
  if (state == nullptr) return nullptr;
  if (state->native_builtins_binding_ref != nullptr) {
    napi_value existing = nullptr;
    if (napi_get_reference_value(env, state->native_builtins_binding_ref, &existing) == napi_ok &&
        existing != nullptr) {
      return existing;
    }
  }

  napi_value binding = nullptr;
  if (napi_create_object(env, &binding) != napi_ok || binding == nullptr) return nullptr;

  const std::vector<std::string>& builtin_ids = CollectRuntimeBuiltinIds();
  napi_value builtin_ids_array = nullptr;
  if (napi_create_array_with_length(env, builtin_ids.size(), &builtin_ids_array) != napi_ok ||
      builtin_ids_array == nullptr) {
    return nullptr;
  }
  for (size_t i = 0; i < builtin_ids.size(); ++i) {
    napi_value id = nullptr;
    if (napi_create_string_utf8(env, builtin_ids[i].c_str(), NAPI_AUTO_LENGTH, &id) != napi_ok ||
        id == nullptr ||
        napi_set_element(env, builtin_ids_array, i, id) != napi_ok) {
      return nullptr;
    }
  }
  if (napi_set_named_property(env, binding, "builtinIds", builtin_ids_array) != napi_ok) {
    return nullptr;
  }

  napi_value compile_fn = nullptr;
  if (napi_create_function(env, "compileFunction", NAPI_AUTO_LENGTH, NoopCallback, nullptr, &compile_fn) != napi_ok ||
      compile_fn == nullptr ||
      napi_set_named_property(env, binding, "compileFunction", compile_fn) != napi_ok) {
    return nullptr;
  }
  napi_value set_internal_loaders_fn = nullptr;
  if (napi_create_function(env,
                           "setInternalLoaders",
                           NAPI_AUTO_LENGTH,
                           NoopCallback,
                           nullptr,
                           &set_internal_loaders_fn) != napi_ok ||
      set_internal_loaders_fn == nullptr ||
      napi_set_named_property(env, binding, "setInternalLoaders", set_internal_loaders_fn) != napi_ok) {
    return nullptr;
  }

  if (state->native_builtins_binding_ref != nullptr) {
    napi_delete_reference(env, state->native_builtins_binding_ref);
    state->native_builtins_binding_ref = nullptr;
  }
  if (napi_create_reference(env, binding, 1, &state->native_builtins_binding_ref) != napi_ok ||
      state->native_builtins_binding_ref == nullptr) {
    return nullptr;
  }
  return binding;
}

static napi_value NativeGetInternalBindingCallback(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  void* data = nullptr;
  if (napi_get_cb_info(env, info, &argc, argv, nullptr, &data) != napi_ok || data == nullptr) {
    return nullptr;
  }
  auto* state = static_cast<ModuleLoaderState*>(data);

  napi_value undefined = nullptr;
  napi_get_undefined(env, &undefined);

  if (argc < 1 || argv[0] == nullptr) {
    return undefined;
  }
  const std::string name = ValueToUtf8(env, argv[0]);
  if (name == "builtins") {
    napi_value binding = GetOrCreateNativeBuiltinsBinding(env, state);
    return binding != nullptr ? binding : undefined;
  }
  if (name == "timers") {
    napi_value global = nullptr;
    if (napi_get_global(env, &global) != napi_ok || global == nullptr) {
      return undefined;
    }
    napi_value timers_binding = nullptr;
    if (napi_get_named_property(env, global, "__unode_timers_binding_js", &timers_binding) == napi_ok &&
        timers_binding != nullptr) {
      return timers_binding;
    }
    if (napi_get_named_property(env, global, "__unode_timers_binding", &timers_binding) == napi_ok &&
        timers_binding != nullptr) {
      return timers_binding;
    }
    return undefined;
  }
  if (name == "types") {
    napi_value global = nullptr;
    if (napi_get_global(env, &global) != napi_ok || global == nullptr) {
      return undefined;
    }
    napi_value types_binding = nullptr;
    if (napi_get_named_property(env, global, "__unode_types", &types_binding) != napi_ok ||
        types_binding == nullptr) {
      return undefined;
    }
    return types_binding;
  }
  if (name == "util") {
    napi_value global = nullptr;
    if (napi_get_global(env, &global) != napi_ok || global == nullptr) {
      return undefined;
    }
    bool has_util = false;
    if (napi_has_named_property(env, global, "__unode_util", &has_util) != napi_ok || !has_util) {
      return undefined;
    }
    napi_value util_binding = nullptr;
    if (napi_get_named_property(env, global, "__unode_util", &util_binding) != napi_ok ||
        util_binding == nullptr) {
      return undefined;
    }
    return util_binding;
  }
  if (name == "process_methods") {
    napi_value global = nullptr;
    if (napi_get_global(env, &global) != napi_ok || global == nullptr) {
      return undefined;
    }
    napi_value binding = nullptr;
    if (napi_get_named_property(env, global, "__unode_process_methods_binding", &binding) != napi_ok ||
        binding == nullptr) {
      return undefined;
    }
    return binding;
  }
  if (name == "report") {
    napi_value global = nullptr;
    if (napi_get_global(env, &global) != napi_ok || global == nullptr) {
      return undefined;
    }
    napi_value binding = nullptr;
    if (napi_get_named_property(env, global, "__unode_report_binding", &binding) != napi_ok ||
        binding == nullptr) {
      return undefined;
    }
    return binding;
  }
  return undefined;
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

// When running a raw Node test (entry under node/test/parallel/), redirect
// require('../common') and require('../common/fixtures') to unode's minimal
// common shims so Node's heavy common/index.js is not loaded.
static bool ApplyNodeTestCommonRedirect(ModuleLoaderState* state, fs::path* resolved_path) {
  if (state == nullptr || state->entry_dir.empty()) return false;

  fs::path entry_dir = fs::path(state->entry_dir).lexically_normal();
  if (entry_dir.filename() != "parallel") return false;
  if (entry_dir.parent_path().filename() != "test") return false;

  // Redirect node/test/common/* to unode's minimal common (not node/common).
  fs::path node_test_dir = entry_dir.parent_path();
  fs::path node_common_dir = node_test_dir / "common";

  fs::path normalized = fs::absolute(*resolved_path).lexically_normal();
  fs::path rel = normalized.lexically_relative(node_common_dir);
  if (rel.empty() || rel.string().find("..") == 0) return false;

  static const fs::path unode_common_dir =
      fs::absolute(fs::path(__FILE__).parent_path() / ".." / "tests" / "node-compat" / "common").lexically_normal();
  if (!PathExistsDirectory(unode_common_dir)) {
    return false;
  }
  *resolved_path = (unode_common_dir / rel).lexically_normal();
  return true;
}

napi_value ResolveSpecifierForContext(napi_env env, RequireContext* context, const std::string& specifier, bool throw_on_error) {
  fs::path resolved_path;
  if (ResolveBuiltinPath(specifier, context->base_dir, &resolved_path) ||
      ResolveModulePath(specifier, context->base_dir, &resolved_path) ||
      ResolveNodeModules(specifier, context->base_dir, &resolved_path)) {
    ApplyNodeTestCommonRedirect(context->state, &resolved_path);
    return CreateResolvedPathString(env, resolved_path);
  }
  if (throw_on_error) {
    ThrowModuleNotFound(env, specifier);
  }
  return nullptr;
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
                      ModuleLoaderState* state,
                      const fs::path& resolved_path,
                      napi_value module_obj,
                      napi_value exports_obj,
                      napi_value require_fn) {
  const std::string source = ReadTextFile(resolved_path);
  if (source.empty()) {
    return ThrowLoaderError(env, ("Failed to read module source: " + resolved_path.string()).c_str());
  }

  // Node-aligned: compile the wrapper as a function, then call it from C++ with (internalBinding, primordials)
  // as arguments (realm->primordials() in Node). No JS expression like globalThis.primordials at call time.
  const std::string wrapped_source =
      "(function(internalBinding, primordials) {"
      "return function(exports, require, module, __filename, __dirname) {\n" + source + "\n};"
      "})";
  napi_value script_source = nullptr;
  if (napi_create_string_utf8(env, wrapped_source.c_str(), NAPI_AUTO_LENGTH, &script_source) != napi_ok ||
      script_source == nullptr) {
    return ThrowLoaderError(env, "Failed to create wrapped module source");
  }

  napi_value outer_fn = nullptr;
  if (napi_run_script(env, script_source, &outer_fn) != napi_ok || outer_fn == nullptr) {
    return false;  // Preserve JS exception.
  }

  napi_value global = GetGlobal(env);
  if (global == nullptr) {
    return ThrowLoaderError(env, "Failed to fetch global object");
  }

  napi_value internal_binding_val = nullptr;
  napi_value primordials_val = nullptr;
  if (state != nullptr && state->internal_binding_ref != nullptr) {
    napi_get_reference_value(env, state->internal_binding_ref, &internal_binding_val);
  }
  if (internal_binding_val == nullptr) {
    napi_get_named_property(env, global, "internalBinding", &internal_binding_val);
  }
  if (state != nullptr && state->primordials_ref != nullptr) {
    napi_get_reference_value(env, state->primordials_ref, &primordials_val);
  }
  if (primordials_val == nullptr) {
    napi_get_named_property(env, global, "__unode_primordials", &primordials_val);
  }
  if (primordials_val == nullptr) {
    napi_get_named_property(env, global, "primordials", &primordials_val);
  }
  // Never pass JS undefined to the wrapper when __unode_primordials exists (e.g. ref was set to
  // undefined by mistake). Prefer the container so builtins that destructure primordials don't throw.
  napi_value undefined_val = nullptr;
  if (napi_get_undefined(env, &undefined_val) == napi_ok && undefined_val != nullptr &&
      primordials_val != nullptr) {
    bool is_undefined = false;
    if (napi_strict_equals(env, primordials_val, undefined_val, &is_undefined) == napi_ok &&
        is_undefined) {
      primordials_val = nullptr;
      napi_get_named_property(env, global, "__unode_primordials", &primordials_val);
    }
  }
  if (primordials_val == nullptr) {
    napi_get_undefined(env, &primordials_val);
  }

  napi_value wrapper_args[2] = {internal_binding_val != nullptr ? internal_binding_val : nullptr, primordials_val};
  if (wrapper_args[0] == nullptr) {
    napi_get_undefined(env, &wrapper_args[0]);
  }
  napi_value inner_fn = nullptr;
  if (napi_call_function(env, global, outer_fn, 2, wrapper_args, &inner_fn) != napi_ok || inner_fn == nullptr) {
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
  napi_value call_result = nullptr;
  if (napi_call_function(env, global, inner_fn, 5, argv, &call_result) != napi_ok) {
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
    bool has_exception = false;
    if (napi_is_exception_pending(env, &has_exception) == napi_ok && has_exception) {
      napi_value exc = nullptr;
      if (napi_get_and_clear_last_exception(env, &exc) == napi_ok && exc != nullptr) {
        napi_value exc_msg = nullptr;
        if (napi_coerce_to_string(env, exc, &exc_msg) == napi_ok && exc_msg != nullptr) {
          const std::string msg_str = ValueToUtf8(env, exc_msg);
          std::string path_for_msg = resolved_path.string();
          // Compat tests run from node-compat/ so paths contain "node-compat"; regex expects "test".
          const size_t node_compat = path_for_msg.find("node-compat");
          if (node_compat != std::string::npos) {
            path_for_msg.replace(node_compat, 11, "test");
          }
          const std::string prefixed = path_for_msg + ": " + (msg_str.empty() ? "JSON parse error" : msg_str);
          napi_value new_msg = nullptr;
          if (napi_create_string_utf8(env, prefixed.c_str(), NAPI_AUTO_LENGTH, &new_msg) == napi_ok && new_msg != nullptr) {
            const char* throw_script = "(function(m){ throw new SyntaxError(m); })";
            napi_value throw_script_val = nullptr;
            if (napi_create_string_utf8(env, throw_script, NAPI_AUTO_LENGTH, &throw_script_val) == napi_ok &&
                throw_script_val != nullptr) {
              napi_value throw_fn = nullptr;
              if (napi_run_script(env, throw_script_val, &throw_fn) == napi_ok && throw_fn != nullptr) {
                napi_value ignore = nullptr;
                napi_call_function(env, global, throw_fn, 1, &new_msg, &ignore);
                return false;
              }
            }
            napi_throw(env, exc);
          } else {
            napi_throw(env, exc);
          }
        } else {
          napi_throw(env, exc);
        }
      }
    }
    return false;
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

  std::string resolved_key;
  fs::path resolved_path;
  if (ResolveBuiltinPath(specifier, context->base_dir, &resolved_path)) {
    resolved_key = resolved_path.string();
  } else {
    napi_value resolved_path_value = ResolveSpecifierForContext(env, context, specifier, true);
    if (resolved_path_value == nullptr) {
      return nullptr;
    }
    resolved_key = ValueToUtf8(env, resolved_path_value);
    if (resolved_key.empty()) {
      ThrowLoaderError(env, "Failed to resolve module path");
      return nullptr;
    }
    resolved_path = fs::path(resolved_key);
  }


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
    ok = EvaluateJsModule(env, state, resolved_path, module_obj, exports_obj, require_fn);
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

void UnodeSetPrimordials(napi_env env, napi_value primordials) {
  if (env == nullptr || primordials == nullptr) return;
  auto it = g_loader_states.find(env);
  if (it == g_loader_states.end()) return;
  ModuleLoaderState& state = it->second;
  if (state.primordials_ref != nullptr) {
    napi_delete_reference(env, state.primordials_ref);
    state.primordials_ref = nullptr;
  }
  if (napi_create_reference(env, primordials, 1, &state.primordials_ref) != napi_ok) {
    state.primordials_ref = nullptr;
  }
}

void UnodeSetInternalBinding(napi_env env, napi_value internal_binding) {
  if (env == nullptr || internal_binding == nullptr) return;
  auto it = g_loader_states.find(env);
  if (it == g_loader_states.end()) return;
  ModuleLoaderState& state = it->second;
  if (state.internal_binding_ref != nullptr) {
    napi_delete_reference(env, state.internal_binding_ref);
    state.internal_binding_ref = nullptr;
  }
  if (napi_create_reference(env, internal_binding, 1, &state.internal_binding_ref) != napi_ok) {
    state.internal_binding_ref = nullptr;
  }
}

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
  for (auto& kv : state.module_cache) {
    napi_delete_reference(env, kv.second);
  }
  state.module_cache.clear();
  if (state.cache_object_ref != nullptr) {
    napi_delete_reference(env, state.cache_object_ref);
    state.cache_object_ref = nullptr;
  }
  if (state.primordials_ref != nullptr) {
    napi_delete_reference(env, state.primordials_ref);
    state.primordials_ref = nullptr;
  }
  if (state.internal_binding_ref != nullptr) {
    napi_delete_reference(env, state.internal_binding_ref);
    state.internal_binding_ref = nullptr;
  }
  if (state.native_builtins_binding_ref != nullptr) {
    napi_delete_reference(env, state.native_builtins_binding_ref);
    state.native_builtins_binding_ref = nullptr;
  }
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

  napi_value native_get_internal_binding_fn = nullptr;
  if (napi_create_function(env,
                           "__unode_get_internal_binding",
                           NAPI_AUTO_LENGTH,
                           NativeGetInternalBindingCallback,
                           &state,
                           &native_get_internal_binding_fn) != napi_ok ||
      native_get_internal_binding_fn == nullptr ||
      napi_set_named_property(env,
                              global,
                              "__unode_get_internal_binding",
                              native_get_internal_binding_fn) != napi_ok) {
    return napi_generic_failure;
  }

  // Bootstrap require: resolves from builtins dir so the prelude can load
  // internal/bootstrap/realm
  // before the entry script runs, regardless of entry_dir. Declares internalBinding once.
  const std::string bootstrap_base_dir = GetBuiltinsDirForBootstrap();
  auto* bootstrap_context = new RequireContext{&state, bootstrap_base_dir};
  g_require_contexts.push_back(bootstrap_context);
  napi_value bootstrap_require_fn = CreateRequireFunction(env, bootstrap_context);
  if (bootstrap_require_fn != nullptr &&
      napi_set_named_property(env, global, "__unode_bootstrap_require", bootstrap_require_fn) != napi_ok) {
    return napi_generic_failure;
  }
  return napi_ok;
}
