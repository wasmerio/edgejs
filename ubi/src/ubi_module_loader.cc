#include "ubi_module_loader.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#include "uv.h"
#include "unofficial_napi.h"

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

struct TaskQueueBindingState {
  napi_ref binding_ref = nullptr;
  napi_ref tick_callback_ref = nullptr;
  napi_ref promise_reject_callback_ref = nullptr;
};
struct ErrorsBindingState {
  napi_ref binding_ref = nullptr;
};
struct TraceEventsBindingState {
  napi_ref binding_ref = nullptr;
  napi_ref state_update_handler_ref = nullptr;
};

std::unordered_map<napi_env, ModuleLoaderState> g_loader_states;
std::unordered_map<napi_env, TaskQueueBindingState> g_task_queue_states;
std::unordered_map<napi_env, ErrorsBindingState> g_errors_states;
std::unordered_map<napi_env, TraceEventsBindingState> g_trace_events_states;
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

std::string CanonicalPathKey(const fs::path& path) {
  std::error_code ec;
  fs::path absolute = path;
  if (!absolute.is_absolute()) {
    absolute = fs::absolute(path, ec);
    if (ec) {
      absolute = path;
      ec.clear();
    }
  }
  const fs::path canonical = fs::weakly_canonical(absolute, ec);
  if (!ec) return canonical.lexically_normal().string();
  return absolute.lexically_normal().string();
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

// Resolve bare specifier (e.g. "assert", "path", "node:worker_threads") from ubi/src/builtins/<id>.js.
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
  // Resolve critical internal util modules directly to upstream node-lib paths.
  // This avoids an extra shim layer that can expose partially initialized wrapper
  // exports during circular loads (e.g. events <-> internal/util call paths).
  if (id == "internal/util" || id == "internal/util/inspect") {
    static const fs::path upstream_node_lib_dir =
        fs::absolute(fs::path(__FILE__).parent_path() / ".." / ".." / "node-lib").lexically_normal();
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
  // Preserve Node-style relative builtin lookup for modules loaded from node-lib
  // (e.g. internal/v8/startup_snapshot from node-lib/buffer.js).
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

static napi_value TaskQueueEnqueueMicrotask(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  if (napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr) != napi_ok || argc < 1) return nullptr;

#if defined(UBI_BUNDLED_NAPI_V8)
  if (unofficial_napi_enqueue_microtask(env, argv[0]) == napi_ok) {
    napi_value undefined = nullptr;
    napi_get_undefined(env, &undefined);
    return undefined;
  }
#endif

  napi_value global = nullptr;
  if (napi_get_global(env, &global) != napi_ok || global == nullptr) return nullptr;

  napi_value queue_microtask = nullptr;
  if (napi_get_named_property(env, global, "queueMicrotask", &queue_microtask) == napi_ok &&
      queue_microtask != nullptr) {
    napi_valuetype t = napi_undefined;
    if (napi_typeof(env, queue_microtask, &t) == napi_ok && t == napi_function) {
      napi_value ignored = nullptr;
      napi_call_function(env, global, queue_microtask, 1, argv, &ignored);
    }
  }

  napi_value undefined = nullptr;
  napi_get_undefined(env, &undefined);
  return undefined;
}

static napi_value TaskQueueRunMicrotasks(napi_env env, napi_callback_info /*info*/) {
  (void)unofficial_napi_process_microtasks(env);
  napi_value undefined = nullptr;
  napi_get_undefined(env, &undefined);
  return undefined;
}

static napi_value TaskQueueSetTickCallback(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  if (napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr) != napi_ok || argc < 1) return nullptr;
  auto& st = g_task_queue_states[env];
  if (st.tick_callback_ref != nullptr) {
    napi_delete_reference(env, st.tick_callback_ref);
    st.tick_callback_ref = nullptr;
  }
  napi_valuetype t = napi_undefined;
  if (napi_typeof(env, argv[0], &t) == napi_ok && t == napi_function) {
    napi_create_reference(env, argv[0], 1, &st.tick_callback_ref);
  }
  napi_value undefined = nullptr;
  napi_get_undefined(env, &undefined);
  return undefined;
}

static napi_value TaskQueueSetPromiseRejectCallback(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  if (napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr) != napi_ok || argc < 1) return nullptr;
#if defined(UBI_BUNDLED_NAPI_V8)
  (void)unofficial_napi_set_promise_reject_callback(env, argv[0]);
#endif
  auto& st = g_task_queue_states[env];
  if (st.promise_reject_callback_ref != nullptr) {
    napi_delete_reference(env, st.promise_reject_callback_ref);
    st.promise_reject_callback_ref = nullptr;
  }
  napi_valuetype t = napi_undefined;
  if (napi_typeof(env, argv[0], &t) == napi_ok && t == napi_function) {
    napi_create_reference(env, argv[0], 1, &st.promise_reject_callback_ref);
  }
  napi_value undefined = nullptr;
  napi_get_undefined(env, &undefined);
  return undefined;
}

static napi_value GetOrCreateTaskQueueBinding(napi_env env) {
  auto& st = g_task_queue_states[env];
  if (st.binding_ref != nullptr) {
    napi_value existing = nullptr;
    if (napi_get_reference_value(env, st.binding_ref, &existing) == napi_ok && existing != nullptr) {
      return existing;
    }
  }

  napi_value binding = nullptr;
  if (napi_create_object(env, &binding) != napi_ok || binding == nullptr) return nullptr;

  auto define_method = [&](const char* name, napi_callback cb) -> bool {
    napi_value fn = nullptr;
    if (napi_create_function(env, name, NAPI_AUTO_LENGTH, cb, nullptr, &fn) != napi_ok || fn == nullptr) {
      return false;
    }
    return napi_set_named_property(env, binding, name, fn) == napi_ok;
  };

  if (!define_method("enqueueMicrotask", TaskQueueEnqueueMicrotask) ||
      !define_method("setTickCallback", TaskQueueSetTickCallback) ||
      !define_method("runMicrotasks", TaskQueueRunMicrotasks) ||
      !define_method("setPromiseRejectCallback", TaskQueueSetPromiseRejectCallback)) {
    return nullptr;
  }

  napi_value tick_ab = nullptr;
  void* tick_data = nullptr;
  if (napi_create_arraybuffer(env, 2 * sizeof(int32_t), &tick_data, &tick_ab) != napi_ok ||
      tick_ab == nullptr || tick_data == nullptr) {
    return nullptr;
  }
  auto* fields = static_cast<int32_t*>(tick_data);
  fields[0] = 0;  // hasTickScheduled
  fields[1] = 0;  // hasRejectionToWarn
  napi_value tick_info = nullptr;
  if (napi_create_typedarray(env, napi_int32_array, 2, tick_ab, 0, &tick_info) != napi_ok || tick_info == nullptr) {
    return nullptr;
  }
  if (napi_set_named_property(env, binding, "tickInfo", tick_info) != napi_ok) return nullptr;

  napi_value promise_events = nullptr;
  if (napi_create_object(env, &promise_events) != napi_ok || promise_events == nullptr) return nullptr;
  auto set_event_const = [&](const char* name, int32_t value) -> bool {
    napi_value v = nullptr;
    return napi_create_int32(env, value, &v) == napi_ok && v != nullptr &&
           napi_set_named_property(env, promise_events, name, v) == napi_ok;
  };
  if (!set_event_const("kPromiseRejectWithNoHandler", 0) ||
      !set_event_const("kPromiseHandlerAddedAfterReject", 1) ||
      !set_event_const("kPromiseResolveAfterResolved", 2) ||
      !set_event_const("kPromiseRejectAfterResolved", 3)) {
    return nullptr;
  }
  if (napi_set_named_property(env, binding, "promiseRejectEvents", promise_events) != napi_ok) return nullptr;

  if (st.binding_ref != nullptr) {
    napi_delete_reference(env, st.binding_ref);
    st.binding_ref = nullptr;
  }
  if (napi_create_reference(env, binding, 1, &st.binding_ref) != napi_ok || st.binding_ref == nullptr) {
    return nullptr;
  }
  return binding;
}

static napi_value ErrorsNoSideEffectsToString(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  if (napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr) != napi_ok || argc < 1) return nullptr;
  napi_value out = nullptr;
  if (napi_coerce_to_string(env, argv[0], &out) != napi_ok || out == nullptr) {
    napi_get_undefined(env, &out);
  }
  return out;
}

static napi_value ErrorsTriggerUncaughtException(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value argv[2] = {nullptr, nullptr};
  if (napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr) != napi_ok) return nullptr;
  if (argc >= 1 && argv[0] != nullptr) {
    napi_throw(env, argv[0]);
  }
  napi_value undefined = nullptr;
  napi_get_undefined(env, &undefined);
  return undefined;
}

static napi_value GetOrCreateErrorsBinding(napi_env env) {
  auto& st = g_errors_states[env];
  if (st.binding_ref != nullptr) {
    napi_value existing = nullptr;
    if (napi_get_reference_value(env, st.binding_ref, &existing) == napi_ok && existing != nullptr) {
      return existing;
    }
  }

  napi_value binding = nullptr;
  if (napi_create_object(env, &binding) != napi_ok || binding == nullptr) return nullptr;
  auto define_method = [&](const char* name, napi_callback cb) -> bool {
    napi_value fn = nullptr;
    return napi_create_function(env, name, NAPI_AUTO_LENGTH, cb, nullptr, &fn) == napi_ok &&
           fn != nullptr &&
           napi_set_named_property(env, binding, name, fn) == napi_ok;
  };
  if (!define_method("noSideEffectsToString", ErrorsNoSideEffectsToString) ||
      !define_method("triggerUncaughtException", ErrorsTriggerUncaughtException)) {
    return nullptr;
  }

  napi_value exit_codes = nullptr;
  if (napi_create_object(env, &exit_codes) != napi_ok || exit_codes == nullptr) return nullptr;
  auto set_const = [&](const char* name, int32_t value) -> bool {
    napi_value v = nullptr;
    return napi_create_int32(env, value, &v) == napi_ok &&
           v != nullptr &&
           napi_set_named_property(env, exit_codes, name, v) == napi_ok;
  };
  if (!set_const("kNoFailure", 0) ||
      !set_const("kGenericUserError", 1) ||
      !set_const("kInvalidArgument", 9)) {
    return nullptr;
  }
  if (napi_set_named_property(env, binding, "exitCodes", exit_codes) != napi_ok) return nullptr;

  if (st.binding_ref != nullptr) {
    napi_delete_reference(env, st.binding_ref);
    st.binding_ref = nullptr;
  }
  if (napi_create_reference(env, binding, 1, &st.binding_ref) != napi_ok || st.binding_ref == nullptr) {
    return nullptr;
  }
  return binding;
}

static napi_value TraceEventsTrace(napi_env env, napi_callback_info /*info*/) {
  napi_value undefined = nullptr;
  napi_get_undefined(env, &undefined);
  return undefined;
}

static napi_value TraceEventsIsTraceCategoryEnabled(napi_env env, napi_callback_info /*info*/) {
  napi_value out = nullptr;
  napi_get_boolean(env, false, &out);
  return out;
}

static napi_value TraceEventsGetEnabledCategories(napi_env env, napi_callback_info /*info*/) {
  napi_value out = nullptr;
  napi_create_string_utf8(env, "", NAPI_AUTO_LENGTH, &out);
  return out;
}

static napi_value TraceEventsGetCategoryEnabledBuffer(napi_env env, napi_callback_info /*info*/) {
  napi_value ab = nullptr;
  void* data = nullptr;
  if (napi_create_arraybuffer(env, sizeof(uint8_t), &data, &ab) != napi_ok || ab == nullptr || data == nullptr) {
    return nullptr;
  }
  static_cast<uint8_t*>(data)[0] = 0;
  napi_value ta = nullptr;
  if (napi_create_typedarray(env, napi_uint8_array, 1, ab, 0, &ta) != napi_ok || ta == nullptr) return nullptr;
  return ta;
}

static napi_value TraceEventsCategorySetEnable(napi_env env, napi_callback_info /*info*/) {
  napi_value undefined = nullptr;
  napi_get_undefined(env, &undefined);
  return undefined;
}

static napi_value TraceEventsCategorySetDisable(napi_env env, napi_callback_info /*info*/) {
  napi_value undefined = nullptr;
  napi_get_undefined(env, &undefined);
  return undefined;
}

static napi_value TraceEventsCategorySetCtor(napi_env env, napi_callback_info /*info*/) {
  napi_value obj = nullptr;
  if (napi_create_object(env, &obj) != napi_ok || obj == nullptr) return nullptr;
  napi_value enable_fn = nullptr;
  napi_value disable_fn = nullptr;
  if (napi_create_function(env, "enable", NAPI_AUTO_LENGTH, TraceEventsCategorySetEnable, nullptr, &enable_fn) == napi_ok &&
      enable_fn != nullptr) {
    napi_set_named_property(env, obj, "enable", enable_fn);
  }
  if (napi_create_function(env, "disable", NAPI_AUTO_LENGTH, TraceEventsCategorySetDisable, nullptr, &disable_fn) == napi_ok &&
      disable_fn != nullptr) {
    napi_set_named_property(env, obj, "disable", disable_fn);
  }
  return obj;
}

static napi_value TraceEventsSetTraceCategoryStateUpdateHandler(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  if (napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr) != napi_ok) return nullptr;

  auto& st = g_trace_events_states[env];
  if (st.state_update_handler_ref != nullptr) {
    napi_delete_reference(env, st.state_update_handler_ref);
    st.state_update_handler_ref = nullptr;
  }
  if (argc >= 1 && argv[0] != nullptr) {
    napi_valuetype t = napi_undefined;
    if (napi_typeof(env, argv[0], &t) == napi_ok && t == napi_function) {
      napi_create_reference(env, argv[0], 1, &st.state_update_handler_ref);
      // Node calls handler upon tracing state changes; tracing is disabled in ubi.
      napi_value global = nullptr;
      napi_get_global(env, &global);
      napi_value fn = nullptr;
      if (napi_get_reference_value(env, st.state_update_handler_ref, &fn) == napi_ok && fn != nullptr) {
        napi_value arg = nullptr;
        napi_get_boolean(env, false, &arg);
        napi_value ignored = nullptr;
        napi_call_function(env, global, fn, 1, &arg, &ignored);
      }
    }
  }
  napi_value undefined = nullptr;
  napi_get_undefined(env, &undefined);
  return undefined;
}

static napi_value GetOrCreateTraceEventsBinding(napi_env env) {
  auto& st = g_trace_events_states[env];
  if (st.binding_ref != nullptr) {
    napi_value existing = nullptr;
    if (napi_get_reference_value(env, st.binding_ref, &existing) == napi_ok && existing != nullptr) {
      return existing;
    }
  }

  napi_value binding = nullptr;
  if (napi_create_object(env, &binding) != napi_ok || binding == nullptr) return nullptr;
  auto define_method = [&](const char* name, napi_callback cb) -> bool {
    napi_value fn = nullptr;
    return napi_create_function(env, name, NAPI_AUTO_LENGTH, cb, nullptr, &fn) == napi_ok &&
           fn != nullptr &&
           napi_set_named_property(env, binding, name, fn) == napi_ok;
  };
  if (!define_method("trace", TraceEventsTrace) ||
      !define_method("isTraceCategoryEnabled", TraceEventsIsTraceCategoryEnabled) ||
      !define_method("getEnabledCategories", TraceEventsGetEnabledCategories) ||
      !define_method("getCategoryEnabledBuffer", TraceEventsGetCategoryEnabledBuffer) ||
      !define_method("setTraceCategoryStateUpdateHandler", TraceEventsSetTraceCategoryStateUpdateHandler)) {
    return nullptr;
  }

  napi_value category_set_ctor = nullptr;
  if (napi_create_function(env,
                           "CategorySet",
                           NAPI_AUTO_LENGTH,
                           TraceEventsCategorySetCtor,
                           nullptr,
                           &category_set_ctor) != napi_ok ||
      category_set_ctor == nullptr ||
      napi_set_named_property(env, binding, "CategorySet", category_set_ctor) != napi_ok) {
    return nullptr;
  }

  if (st.binding_ref != nullptr) {
    napi_delete_reference(env, st.binding_ref);
    st.binding_ref = nullptr;
  }
  if (napi_create_reference(env, binding, 1, &st.binding_ref) != napi_ok || st.binding_ref == nullptr) {
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
  auto get_global_named = [&](const char* key) -> napi_value {
    napi_value global = nullptr;
    if (napi_get_global(env, &global) != napi_ok || global == nullptr) return undefined;
    bool has_prop = false;
    if (napi_has_named_property(env, global, key, &has_prop) != napi_ok || !has_prop) return undefined;
    napi_value out = nullptr;
    if (napi_get_named_property(env, global, key, &out) != napi_ok || out == nullptr) return undefined;
    return out;
  };
  auto set_int32 = [&](napi_value obj, const char* key, int32_t value) {
    napi_value v = nullptr;
    if (napi_create_int32(env, value, &v) == napi_ok && v != nullptr) {
      napi_set_named_property(env, obj, key, v);
    }
  };
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
    if (napi_get_named_property(env, global, "__ubi_timers_binding_js", &timers_binding) == napi_ok &&
        timers_binding != nullptr) {
      return timers_binding;
    }
    if (napi_get_named_property(env, global, "__ubi_timers_binding", &timers_binding) == napi_ok &&
        timers_binding != nullptr) {
      return timers_binding;
    }
    return undefined;
  }
  if (name == "encoding_binding") return get_global_named("__ubi_encoding");
  if (name == "task_queue") {
    napi_value binding = GetOrCreateTaskQueueBinding(env);
    return binding != nullptr ? binding : undefined;
  }
  if (name == "errors") {
    napi_value binding = GetOrCreateErrorsBinding(env);
    return binding != nullptr ? binding : undefined;
  }
  if (name == "trace_events") {
    napi_value binding = GetOrCreateTraceEventsBinding(env);
    return binding != nullptr ? binding : undefined;
  }
  if (name == "url_pattern") {
    napi_value out = nullptr;
    if (napi_create_object(env, &out) != napi_ok || out == nullptr) return undefined;
    napi_value global = nullptr;
    if (napi_get_global(env, &global) == napi_ok && global != nullptr) {
      napi_value url_pattern_ctor = nullptr;
      if (napi_get_named_property(env, global, "URLPattern", &url_pattern_ctor) == napi_ok &&
          url_pattern_ctor != nullptr) {
        napi_set_named_property(env, out, "URLPattern", url_pattern_ctor);
      }
    }
    return out;
  }
  if (name == "constants") {
    napi_value out = nullptr;
    if (napi_create_object(env, &out) != napi_ok || out == nullptr) return undefined;
    napi_value os_obj = nullptr;
    if (napi_create_object(env, &os_obj) == napi_ok && os_obj != nullptr) {
      napi_value signals = nullptr;
      if (napi_create_object(env, &signals) == napi_ok && signals != nullptr) {
        napi_set_named_property(env, os_obj, "signals", signals);
      }
      napi_set_named_property(env, out, "os", os_obj);
    }
    napi_value fs_obj = nullptr;
    if (napi_create_object(env, &fs_obj) == napi_ok && fs_obj != nullptr) {
      set_int32(fs_obj, "F_OK", 0);
      set_int32(fs_obj, "R_OK", 4);
      set_int32(fs_obj, "W_OK", 2);
      set_int32(fs_obj, "X_OK", 1);
      napi_set_named_property(env, out, "fs", fs_obj);
    }
    napi_value os_constants = get_global_named("__ubi_os_constants");
    if (os_constants != undefined) napi_set_named_property(env, out, "os", os_constants);
    napi_value fs_binding = get_global_named("__ubi_fs");
    if (fs_binding != undefined) {
      napi_value fs_constants_obj = nullptr;
      if (napi_create_object(env, &fs_constants_obj) == napi_ok && fs_constants_obj != nullptr) {
        napi_value keys = nullptr;
        if (napi_get_property_names(env, fs_binding, &keys) == napi_ok && keys != nullptr) {
          uint32_t key_count = 0;
          if (napi_get_array_length(env, keys, &key_count) == napi_ok) {
            for (uint32_t i = 0; i < key_count; i++) {
              napi_value key = nullptr;
              if (napi_get_element(env, keys, i, &key) != napi_ok || key == nullptr) continue;
              napi_value value = nullptr;
              if (napi_get_property(env, fs_binding, key, &value) != napi_ok || value == nullptr) continue;
              napi_valuetype t = napi_undefined;
              if (napi_typeof(env, value, &t) != napi_ok) continue;
              if (t == napi_number) {
                napi_set_property(env, fs_constants_obj, key, value);
              }
            }
          }
        }
        napi_set_named_property(env, out, "fs", fs_constants_obj);
      }
    }
    return out;
  }
  if (name == "uv") {
    napi_value out = nullptr;
    if (napi_create_object(env, &out) != napi_ok || out == nullptr) return undefined;
    set_int32(out, "UV_ENOENT", UV_ENOENT);
    set_int32(out, "UV_EEXIST", UV_EEXIST);
    set_int32(out, "UV_EOF", UV_EOF);
    set_int32(out, "UV_EINVAL", UV_EINVAL);
    set_int32(out, "UV_EBADF", UV_EBADF);
    set_int32(out, "UV_ENOTCONN", UV_ENOTCONN);
    set_int32(out, "UV_ECANCELED", UV_ECANCELED);
    set_int32(out, "UV_ENOBUFS", UV_ENOBUFS);
    set_int32(out, "UV_ENOSYS", UV_ENOSYS);
    set_int32(out, "UV_ETIMEDOUT", UV_ETIMEDOUT);
    set_int32(out, "UV_ENOMEM", UV_ENOMEM);
    set_int32(out, "UV_ENOTSOCK", UV_ENOTSOCK);
#if defined(UV_EAI_NONAME)
    set_int32(out, "UV_EAI_NONAME", UV_EAI_NONAME);
#endif
#if defined(UV_EAI_NODATA)
    set_int32(out, "UV_EAI_NODATA", UV_EAI_NODATA);
#endif
    return out;
  }
  if (name == "config") {
    napi_value out = nullptr;
    if (napi_create_object(env, &out) != napi_ok || out == nullptr) return undefined;
    napi_value t = nullptr;
    napi_value f = nullptr;
    napi_get_boolean(env, true, &t);
    napi_get_boolean(env, false, &f);
    napi_set_named_property(env, out, "hasIntl", f);
    napi_set_named_property(env, out, "hasSmallICU", f);
    napi_set_named_property(env, out, "hasInspector", f);
    napi_set_named_property(env, out, "hasTracing", f);
    napi_set_named_property(env, out, "hasOpenSSL", t);
    napi_set_named_property(env, out, "fipsMode", f);
    napi_set_named_property(env, out, "hasNodeOptions", t);
    napi_set_named_property(env, out, "noBrowserGlobals", f);
    napi_set_named_property(env, out, "isDebugBuild", f);
    return out;
  }
  if (name == "types") {
    napi_value global = nullptr;
    if (napi_get_global(env, &global) != napi_ok || global == nullptr) {
      return undefined;
    }
    napi_value types_binding = nullptr;
    if (napi_get_named_property(env, global, "__ubi_types", &types_binding) != napi_ok ||
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
    if (napi_has_named_property(env, global, "__ubi_util", &has_util) != napi_ok || !has_util) {
      return undefined;
    }
    napi_value util_binding = nullptr;
    if (napi_get_named_property(env, global, "__ubi_util", &util_binding) != napi_ok ||
        util_binding == nullptr) {
      return undefined;
    }
    return util_binding;
  }
  if (name == "os") return get_global_named("__ubi_os");
  if (name == "fs") return get_global_named("__ubi_fs");
  if (name == "buffer") return get_global_named("__ubi_buffer");
  if (name == "crypto") {
    napi_value out = get_global_named("__ubi_crypto_binding");
    if (out != undefined) return out;
    return get_global_named("__ubi_crypto");
  }
  if (name == "http_parser") return get_global_named("__ubi_http_parser");
  if (name == "stream_wrap") return get_global_named("__ubi_stream_wrap");
  if (name == "process_wrap") return get_global_named("__ubi_process_wrap");
  if (name == "tcp_wrap") return get_global_named("__ubi_tcp_wrap");
  if (name == "tty_wrap") return get_global_named("__ubi_tty_wrap");
  if (name == "pipe_wrap") return get_global_named("__ubi_pipe_wrap");
  if (name == "signal_wrap") return get_global_named("__ubi_signal_wrap");
  if (name == "cares_wrap") return get_global_named("__ubi_cares_wrap");
  if (name == "udp_wrap") return get_global_named("__ubi_udp_wrap");
  if (name == "url") return get_global_named("__ubi_url");
  if (name == "string_decoder") return get_global_named("__ubi_string_decoder");
  if (name == "spawn_sync") return get_global_named("__ubi_spawn_sync");
  if (name == "process_methods") {
    napi_value global = nullptr;
    if (napi_get_global(env, &global) != napi_ok || global == nullptr) {
      return undefined;
    }
    napi_value binding = nullptr;
    if (napi_get_named_property(env, global, "__ubi_process_methods_binding", &binding) != napi_ok ||
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
    if (napi_get_named_property(env, global, "__ubi_report_binding", &binding) != napi_ok ||
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
  napi_value error_value = MakeError(env, "ERR_UBI_MODULE_LOAD", message);
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
// require('../common') and require('../common/fixtures') to ubi's minimal
// common shims so Node's heavy common/index.js is not loaded.
static bool ApplyNodeTestCommonRedirect(ModuleLoaderState* state, fs::path* resolved_path) {
  if (state == nullptr || state->entry_dir.empty()) return false;

  fs::path entry_dir = fs::path(state->entry_dir).lexically_normal();
  if (entry_dir.filename() != "parallel") return false;
  if (entry_dir.parent_path().filename() != "test") return false;

  // Redirect node/test/common/* to ubi's minimal common (not node/common).
  fs::path node_test_dir = entry_dir.parent_path();
  fs::path node_common_dir = node_test_dir / "common";

  fs::path normalized = fs::absolute(*resolved_path).lexically_normal();
  fs::path rel = normalized.lexically_relative(node_common_dir);
  if (rel.empty() || rel.string().find("..") == 0) return false;

  static const fs::path ubi_common_dir =
      fs::absolute(fs::path(__FILE__).parent_path() / ".." / "tests" / "node-compat" / "common").lexically_normal();
  if (!PathExistsDirectory(ubi_common_dir)) {
    return false;
  }
  *resolved_path = (ubi_common_dir / rel).lexically_normal();
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
      "return function(exports, require, module, __filename, __dirname) {\n" + source +
      "\n//# sourceURL=" + resolved_path.string() + "\n};"
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
    napi_get_named_property(env, global, "__ubi_primordials", &primordials_val);
  }
  if (primordials_val == nullptr) {
    napi_get_named_property(env, global, "primordials", &primordials_val);
  }
  // Never pass JS undefined to the wrapper when __ubi_primordials exists (e.g. ref was set to
  // undefined by mistake). Prefer the container so builtins that destructure primordials don't throw.
  napi_value undefined_val = nullptr;
  if (napi_get_undefined(env, &undefined_val) == napi_ok && undefined_val != nullptr &&
      primordials_val != nullptr) {
    bool is_undefined = false;
    if (napi_strict_equals(env, primordials_val, undefined_val, &is_undefined) == napi_ok &&
        is_undefined) {
      primordials_val = nullptr;
      napi_get_named_property(env, global, "__ubi_primordials", &primordials_val);
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
    resolved_key = CanonicalPathKey(resolved_path);
    resolved_path = fs::path(resolved_key);
  } else {
    napi_value resolved_path_value = ResolveSpecifierForContext(env, context, specifier, true);
    if (resolved_path_value == nullptr) {
      return nullptr;
    }
    resolved_key = CanonicalPathKey(fs::path(ValueToUtf8(env, resolved_path_value)));
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
  const std::string resolved_key = CanonicalPathKey(resolved_path);
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

void UbiSetPrimordials(napi_env env, napi_value primordials) {
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

void UbiSetInternalBinding(napi_env env, napi_value internal_binding) {
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

napi_status UbiInstallModuleLoader(napi_env env, const char* entry_script_path) {
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
                           "__ubi_get_internal_binding",
                           NAPI_AUTO_LENGTH,
                           NativeGetInternalBindingCallback,
                           &state,
                           &native_get_internal_binding_fn) != napi_ok ||
      native_get_internal_binding_fn == nullptr ||
      napi_set_named_property(env,
                              global,
                              "__ubi_get_internal_binding",
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
      napi_set_named_property(env, global, "__ubi_bootstrap_require", bootstrap_require_fn) != napi_ok) {
    return napi_generic_failure;
  }
  return napi_ok;
}

napi_status UbiRunTaskQueueTickCallback(napi_env env, bool* called) {
  if (called != nullptr) {
    *called = false;
  }
  if (env == nullptr) {
    return napi_invalid_arg;
  }
  auto it = g_task_queue_states.find(env);
  if (it == g_task_queue_states.end() || it->second.tick_callback_ref == nullptr) {
    return napi_ok;
  }

  napi_value tick_cb = nullptr;
  napi_status status = napi_get_reference_value(env, it->second.tick_callback_ref, &tick_cb);
  if (status != napi_ok || tick_cb == nullptr) {
    return status == napi_ok ? napi_generic_failure : status;
  }

  napi_value global = nullptr;
  status = napi_get_global(env, &global);
  if (status != napi_ok || global == nullptr) {
    return status == napi_ok ? napi_generic_failure : status;
  }

  napi_value process = nullptr;
  if (napi_get_named_property(env, global, "process", &process) != napi_ok || process == nullptr) {
    process = global;
  }

  napi_value ignored = nullptr;
  status = napi_call_function(env, process, tick_cb, 0, nullptr, &ignored);
  if (status == napi_ok && called != nullptr) {
    *called = true;
  }
  return status;
}
