#include "ubi_module_loader.h"
#include "ubi_errors_binding.h"

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
struct TraceEventsBindingState {
  napi_ref binding_ref = nullptr;
  napi_ref state_update_handler_ref = nullptr;
};

std::unordered_map<napi_env, ModuleLoaderState> g_loader_states;
std::unordered_map<napi_env, TaskQueueBindingState> g_task_queue_states;
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

void ReplaceAll(std::string* text, const std::string& from, const std::string& to) {
  if (text == nullptr || from.empty()) return;
  size_t pos = 0;
  while ((pos = text->find(from, pos)) != std::string::npos) {
    text->replace(pos, from.size(), to);
    pos += to.size();
  }
}

std::string LoadBuiltinsConfigJson() {
  static std::string cached;
  if (!cached.empty()) return cached;

  const fs::path source_root = fs::absolute(fs::path(__FILE__).parent_path() / ".." / "..").lexically_normal();
  const std::vector<fs::path> candidates = {
      source_root / "node" / "config.gypi",
      fs::current_path() / "node" / "config.gypi",
      fs::current_path().parent_path() / "node" / "config.gypi",
  };

  for (const fs::path& candidate : candidates) {
    const std::string raw = ReadTextFile(candidate);
    if (raw.empty()) continue;

    // Drop the generated comment header line and normalize bool-like strings
    // to booleans so JSON.parse() in bootstrap/node matches process.config.
    const size_t newline = raw.find('\n');
    std::string body = (newline == std::string::npos) ? raw : raw.substr(newline + 1);
    ReplaceAll(&body, "\"true\"", "true");
    ReplaceAll(&body, "\"false\"", "false");
    cached = body;
    if (!cached.empty()) return cached;
  }

  cached = "{\"variables\":{\"node_use_amaro\":false}}";
  return cached;
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
  // Internal dep modules (e.g. internal/deps/acorn/*) live under node/deps/*
  // in this workspace, not under ubi/src/builtins.
  if (id.rfind("internal/deps/", 0) == 0) {
    const std::string dep_rel = id.substr(std::string("internal/deps/").size());
    static const fs::path source_root =
        fs::absolute(fs::path(__FILE__).parent_path() / ".." / "..").lexically_normal();
    const std::vector<fs::path> node_deps_roots = {
        source_root / "node" / "deps",
        fs::current_path() / "node" / "deps",
        fs::current_path().parent_path() / "node" / "deps",
    };
    for (const fs::path& deps_root : node_deps_roots) {
      fs::path candidate = deps_root / dep_rel;
      if (ResolveAsFile(candidate, &resolved) || ResolveAsDirectory(candidate, &resolved)) {
        *out = resolved.lexically_normal();
        return true;
      }
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

static napi_value ReturnFalseCallback(napi_env env, napi_callback_info /*info*/) {
  napi_value out = nullptr;
  napi_get_boolean(env, false, &out);
  return out;
}

static napi_value ReturnFirstArgCallback(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  if (napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr) != napi_ok) return nullptr;
  if (argc >= 1 && argv[0] != nullptr) return argv[0];
  napi_value undefined = nullptr;
  napi_get_undefined(env, &undefined);
  return undefined;
}

static napi_value ReturnTrueCallback(napi_env env, napi_callback_info /*info*/) {
  napi_value out = nullptr;
  napi_get_boolean(env, true, &out);
  return out;
}

static napi_value RunScriptAndReturnValue(napi_env env, const char* source) {
  if (env == nullptr || source == nullptr) return nullptr;
  napi_value script = nullptr;
  if (napi_create_string_utf8(env, source, NAPI_AUTO_LENGTH, &script) != napi_ok || script == nullptr) {
    return nullptr;
  }
  napi_value out = nullptr;
  if (napi_run_script(env, script, &out) != napi_ok || out == nullptr) {
    return nullptr;
  }
  return out;
}

static napi_value OptionsGetCLIOptionsValuesCallback(napi_env env, napi_callback_info /*info*/) {
  static constexpr const char* kScript = R"JS((function() {
  const kArrayOptions = new Set([
    '--conditions',
    '--disable-warning',
    '--env-file',
    '--env-file-if-exists',
    '--experimental-loader',
    '--import',
    '--require',
    '--test-coverage-exclude',
    '--test-coverage-include',
    '--test-name-pattern',
    '--test-reporter',
    '--test-reporter-destination',
    '--test-skip-pattern',
    '--watch-path',
  ]);

  const values = Object.assign(Object.create(null), {
    '--abort-on-uncaught-exception': false,
    '--async-context-frame': false,
    '--conditions': [],
    '--diagnostic-dir': '',
    '--disable-warning': [],
    '--dns-result-order': 'verbatim',
    '--enable-source-maps': false,
    '--entry-url': false,
    '--env-file': [],
    '--env-file-if-exists': [],
    '--es-module-specifier-resolution': '',
    '--eval': '',
    '--experimental-addon-modules': false,
    '--experimental-config-file': false,
    '--experimental-default-config-file': false,
    '--experimental-detect-module': false,
    '--experimental-eventsource': false,
    '--experimental-fetch': false,
    '--experimental-global-customevent': false,
    '--experimental-global-webcrypto': false,
    '--experimental-import-meta-resolve': false,
    '--experimental-inspector-network-resource': false,
    '--experimental-loader': [],
    '--experimental-network-inspection': false,
    '--experimental-print-required-tla': false,
    '--experimental-quic': false,
    '--no-experimental-quic': false,
    '--experimental-require-module': false,
    '--experimental-report': false,
    '--experimental-sqlite': false,
    '--no-experimental-sqlite': false,
    '--experimental-test-coverage': false,
    '--experimental-test-module-mocks': false,
    '--experimental-transform-types': false,
    '--experimental-vm-modules': false,
    '--experimental-wasm-modules': false,
    '--experimental-webstorage': false,
    '--experimental-worker': false,
    '--expose-internals': false,
    '--force-fips': false,
    '--frozen-intrinsics': false,
    '--heapsnapshot-near-heap-limit': 0,
    '--heapsnapshot-signal': '',
    '--icu-data-dir': '',
    '--import': [],
    '--insecure-http-parser': false,
    '--input-type': '',
    '--inspect-brk': false,
    '--loader': [],
    '--localstorage-file': '',
    '--max-http-header-size': 16 * 1024,
    '--network-family-autoselection': true,
    '--network-family-autoselection-attempt-timeout': 250,
    '--no-addons': false,
    '--no-deprecation': false,
    '--no-experimental-global-navigator': false,
    '--no-experimental-websocket': false,
    '--node-snapshot': false,
    '--no-node-snapshot': false,
    '--openssl-config': '',
    '--openssl-legacy-provider': false,
    '--openssl-shared-config': false,
    '--pending-deprecation': false,
    '--permission': false,
    '--preserve-symlinks': false,
    '--preserve-symlinks-main': false,
    '--print': false,
    '--redirect-warnings': '',
    '--report-on-signal': false,
    '--require': [],
    '--secure-heap': 0,
    '--secure-heap-min': 0,
    '--stack-trace-limit': 10,
    '--strip-types': false,
    '--test': false,
    '--test-concurrency': 0,
    '--test-coverage-branches': 0,
    '--test-coverage-exclude': [],
    '--test-coverage-functions': 0,
    '--test-coverage-include': [],
    '--test-coverage-lines': 0,
    '--test-force-exit': false,
    '--test-global-setup': '',
    '--test-isolation': 'process',
    '--test-name-pattern': [],
    '--test-only': false,
    '--test-reporter': [],
    '--test-reporter-destination': [],
    '--test-rerun-failures': '',
    '--test-shard': '',
    '--test-skip-pattern': [],
    '--test-timeout': 0,
    '--test-update-snapshots': false,
    '--throw-deprecation': false,
    '--tls-cipher-list': '',
    '--tls-keylog': '',
    '--tls-max-v1.2': false,
    '--tls-max-v1.3': false,
    '--tls-min-v1.0': false,
    '--tls-min-v1.1': false,
    '--tls-min-v1.2': false,
    '--tls-min-v1.3': false,
    '--trace-deprecation': false,
    '--trace-require-module': false,
    '--trace-sigint': false,
    '--trace-tls': false,
    '--trace-warnings': false,
    '--unhandled-rejections': 'throw',
    '--use-bundled-ca': false,
    '--use-env-proxy': false,
    '--use-openssl-ca': false,
    '--use-system-ca': false,
    '--verify-base-objects': false,
    '--no-verify-base-objects': false,
    '--watch': false,
    '--watch-kill-signal': 'SIGTERM',
    '--watch-path': [],
    '--watch-preserve-output': false,
    '--warnings': true,
    '[has_eval_string]': false,
  });

  const lists = [
    Array.isArray(process && process.execArgv) ? process.execArgv : [],
    Array.isArray(process && process.argv) ? process.argv : [],
  ];

  for (const list of lists) {
    for (let i = 0; i < list.length; i++) {
      const token = list[i];
      if (typeof token !== 'string' || token.length === 0 || token[0] !== '-') continue;

      const eq = token.indexOf('=');
      let key = eq === -1 ? token : token.slice(0, eq);
      const raw = eq === -1 ? true : token.slice(eq + 1);

      if (key === '-r') key = '--require';
      if (key === '--loader') key = '--experimental-loader';

      if (kArrayOptions.has(key)) {
        if (!Array.isArray(values[key])) values[key] = [];
        if (eq !== -1) {
          values[key].push(raw);
        } else if (i + 1 < list.length) {
          const next = list[i + 1];
          if (typeof next === 'string' && next.length > 0 && next[0] !== '-') {
            values[key].push(next);
            i++;
          }
        }
        continue;
      }

      if (eq === -1) {
        values[key] = true;
      } else {
        const maybeNum = Number(raw);
        values[key] = Number.isNaN(maybeNum) ? raw : maybeNum;
      }

      if (key === '--eval' || key === '-e') {
        values['[has_eval_string]'] = true;
      }
    }
  }

  return values;
})())JS";
  napi_value out = RunScriptAndReturnValue(env, kScript);
  if (out != nullptr) return out;
  napi_value undefined = nullptr;
  napi_get_undefined(env, &undefined);
  return undefined;
}

static napi_value OptionsGetCLIOptionsInfoCallback(napi_env env, napi_callback_info /*info*/) {
  static constexpr const char* kScript = R"JS((function() {
  const kAllowedInEnvvar = 0;
  const kString = 5;

  function readCliMarkdown() {
    const fs = require('fs');
    const path = require('path');
    const candidates = [
      path.resolve(process.cwd(), 'node/doc/api/cli.md'),
      path.resolve(process.cwd(), '../node/doc/api/cli.md'),
      path.resolve(process.cwd(), '../../node/doc/api/cli.md'),
    ];
    for (const file of candidates) {
      try {
        return fs.readFileSync(file, 'utf8');
      } catch {}
    }
    return '';
  }

  function parseAllowedFlagsFromDocs() {
    const text = readCliMarkdown();
    if (!text) return new Set();
    const sections = [
      ['<!-- node-options-node start -->', '<!-- node-options-node end -->'],
      ['<!-- node-options-v8 start -->', '<!-- node-options-v8 end -->'],
    ];
    const out = new Set();
    for (const section of sections) {
      const start = section[0];
      const end = section[1];
      const re = new RegExp(start + '\\r?\\n([^]*)\\r?\\n' + end);
      const match = text.match(re);
      if (!match) continue;
      const lines = match[1].split(/\\r?\\n/);
      for (const line of lines) {
        if (!line || !line.trim()) continue;
        for (const m of line.matchAll(/`(-[^`]+)`/g)) {
          out.add(m[1].replace('--no-', '--'));
        }
      }
    }
    return out;
  }

  const options = new Map();
  const aliases = new Map();
  const documented = parseAllowedFlagsFromDocs();
  const extras = [
    '--debug-arraybuffer-allocations',
    '--no-debug-arraybuffer-allocations',
    '--es-module-specifier-resolution',
    '--experimental-fetch',
    '--experimental-wasm-modules',
    '--experimental-global-customevent',
    '--experimental-global-webcrypto',
    '--experimental-report',
    '--experimental-worker',
    '--node-snapshot',
    '--no-node-snapshot',
    '--loader',
    '--verify-base-objects',
    '--no-verify-base-objects',
    '--trace-promises',
    '--no-trace-promises',
    '--experimental-quic',
  ];

  for (const opt of documented) {
    options.set(opt, { envVarSettings: kAllowedInEnvvar, type: kString });
  }
  for (const opt of extras) {
    options.set(opt, { envVarSettings: kAllowedInEnvvar, type: kString });
  }
  if (process && process.config && process.config.variables && process.config.variables.node_quic) {
    options.set('--no-experimental-quic', { envVarSettings: kAllowedInEnvvar, type: kString });
  }

  if (!(process && process.features && process.features.inspector)) {
    const inspectorOnly = [
      '--cpu-prof-dir',
      '--cpu-prof-interval',
      '--cpu-prof-name',
      '--cpu-prof',
      '--heap-prof-dir',
      '--heap-prof-interval',
      '--heap-prof-name',
      '--heap-prof',
      '--inspect-brk',
      '--inspect-port',
      '--debug-port',
      '--inspect-publish-uid',
      '--inspect-wait',
      '--inspect',
    ];
    for (const opt of inspectorOnly) options.delete(opt);
  }

  options.set('--perf-basic-prof', { envVarSettings: kAllowedInEnvvar, type: kString });
  options.set('--stack-trace-limit', { envVarSettings: kAllowedInEnvvar, type: kString });
  options.set('-r', { envVarSettings: kAllowedInEnvvar, type: kString });
  if (!(process && process.config && process.config.variables &&
        process.config.variables.v8_enable_i18n_support)) {
    options.delete('--icu-data-dir');
  }

  aliases.set('-r', ['--require']);
  return { options, aliases };
})())JS";
  napi_value out = RunScriptAndReturnValue(env, kScript);
  if (out != nullptr) return out;
  napi_value undefined = nullptr;
  napi_get_undefined(env, &undefined);
  return undefined;
}

static napi_value OptionsGetOptionsAsFlagsCallback(napi_env env, napi_callback_info /*info*/) {
  static constexpr const char* kScript = R"JS((function() {
  return Array.isArray(process && process.execArgv) ? process.execArgv.slice() : [];
})())JS";
  napi_value out = RunScriptAndReturnValue(env, kScript);
  if (out != nullptr) return out;
  napi_value undefined = nullptr;
  napi_get_undefined(env, &undefined);
  return undefined;
}

static napi_value OptionsGetEmbedderOptionsCallback(napi_env env, napi_callback_info /*info*/) {
  static constexpr const char* kScript = R"JS(({
  hasEmbedderPreload: false,
  noBrowserGlobals: false,
  noGlobalSearchPaths: false,
}))JS";
  napi_value out = RunScriptAndReturnValue(env, kScript);
  if (out != nullptr) return out;
  napi_value undefined = nullptr;
  napi_get_undefined(env, &undefined);
  return undefined;
}

static napi_value OptionsGetEnvOptionsInputTypeCallback(napi_env env, napi_callback_info /*info*/) {
  static constexpr const char* kScript = R"JS((function() {
  const map = new Map();
  const stringOpts = [
    '--conditions',
    '--disable-warning',
    '--diagnostic-dir',
    '--dns-result-order',
    '--env-file',
    '--env-file-if-exists',
    '--experimental-loader',
    '--import',
    '--input-type',
    '--max-http-header-size',
    '--network-family-autoselection-attempt-timeout',
    '--redirect-warnings',
    '--require',
    '--secure-heap',
    '--secure-heap-min',
    '--stack-trace-limit',
    '--test-concurrency',
    '--test-coverage-branches',
    '--test-coverage-exclude',
    '--test-coverage-functions',
    '--test-coverage-include',
    '--test-coverage-lines',
    '--test-global-setup',
    '--test-name-pattern',
    '--test-reporter',
    '--test-reporter-destination',
    '--test-rerun-failures',
    '--test-shard',
    '--test-skip-pattern',
    '--test-timeout',
    '--tls-cipher-list',
    '--tls-keylog',
    '--unhandled-rejections',
    '--watch-kill-signal',
    '--watch-path',
  ];
  for (const opt of stringOpts) {
    map.set(opt, 'string');
  }
  return map;
})())JS";
  napi_value out = RunScriptAndReturnValue(env, kScript);
  if (out != nullptr) return out;
  napi_value undefined = nullptr;
  napi_get_undefined(env, &undefined);
  return undefined;
}

static napi_value OptionsGetNamespaceOptionsInputTypeCallback(napi_env env, napi_callback_info /*info*/) {
  static constexpr const char* kScript = R"JS((new Map()))JS";
  napi_value out = RunScriptAndReturnValue(env, kScript);
  if (out != nullptr) return out;
  napi_value undefined = nullptr;
  napi_get_undefined(env, &undefined);
  return undefined;
}

static napi_value ModulesReadPackageJSONCallback(napi_env env, napi_callback_info /*info*/) {
  napi_value undefined = nullptr;
  napi_get_undefined(env, &undefined);
  return undefined;
}

static napi_value ModulesGetNearestParentPackageJSONTypeCallback(napi_env env, napi_callback_info /*info*/) {
  napi_value out = nullptr;
  if (napi_create_string_utf8(env, "none", NAPI_AUTO_LENGTH, &out) != napi_ok || out == nullptr) {
    napi_get_undefined(env, &out);
  }
  return out;
}

static napi_value ModulesGetNearestParentPackageJSONCallback(napi_env env, napi_callback_info /*info*/) {
  napi_value undefined = nullptr;
  napi_get_undefined(env, &undefined);
  return undefined;
}

static napi_value ModulesGetPackageScopeConfigCallback(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  if (napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr) != napi_ok) {
    napi_value undefined = nullptr;
    napi_get_undefined(env, &undefined);
    return undefined;
  }
  if (argc >= 1 && argv[0] != nullptr) return argv[0];
  napi_value out = nullptr;
  if (napi_create_string_utf8(env, "", NAPI_AUTO_LENGTH, &out) != napi_ok || out == nullptr) {
    napi_get_undefined(env, &out);
  }
  return out;
}

static napi_value ModulesGetPackageTypeCallback(napi_env env, napi_callback_info /*info*/) {
  napi_value out = nullptr;
  if (napi_create_string_utf8(env, "none", NAPI_AUTO_LENGTH, &out) != napi_ok || out == nullptr) {
    napi_get_undefined(env, &out);
  }
  return out;
}

static napi_value ModulesEnableCompileCacheCallback(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  if (napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr) != napi_ok) {
    napi_value undefined = nullptr;
    napi_get_undefined(env, &undefined);
    return undefined;
  }
  napi_value out = nullptr;
  if (napi_create_array_with_length(env, 3, &out) != napi_ok || out == nullptr) {
    napi_value undefined = nullptr;
    napi_get_undefined(env, &undefined);
    return undefined;
  }
  napi_value status = nullptr;
  // CompileCacheEnableStatus::DISABLED
  napi_create_int32(env, 3, &status);
  napi_set_element(env, out, 0, status);
  napi_value empty = nullptr;
  napi_create_string_utf8(env, "", NAPI_AUTO_LENGTH, &empty);
  napi_set_element(env, out, 1, empty);
  if (argc >= 1 && argv[0] != nullptr) {
    napi_set_element(env, out, 2, argv[0]);
  } else {
    napi_set_element(env, out, 2, empty);
  }
  return out;
}

static napi_value ModulesGetCompileCacheDirCallback(napi_env env, napi_callback_info /*info*/) {
  napi_value out = nullptr;
  if (napi_create_string_utf8(env, "", NAPI_AUTO_LENGTH, &out) != napi_ok || out == nullptr) {
    napi_get_undefined(env, &out);
  }
  return out;
}

static napi_value ModulesFlushCompileCacheCallback(napi_env env, napi_callback_info /*info*/) {
  napi_value undefined = nullptr;
  napi_get_undefined(env, &undefined);
  return undefined;
}

static napi_value ModulesGetCompileCacheEntryCallback(napi_env env, napi_callback_info /*info*/) {
  napi_value undefined = nullptr;
  napi_get_undefined(env, &undefined);
  return undefined;
}

static napi_value ModulesSaveCompileCacheEntryCallback(napi_env env, napi_callback_info /*info*/) {
  napi_value undefined = nullptr;
  napi_get_undefined(env, &undefined);
  return undefined;
}

static napi_value ModulesSetLazyPathHelpersCallback(napi_env env, napi_callback_info /*info*/) {
  napi_value undefined = nullptr;
  napi_get_undefined(env, &undefined);
  return undefined;
}

static napi_value ContextifyScriptConstructorCallback(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value argv[2] = {nullptr};
  napi_value this_arg = nullptr;
  if (napi_get_cb_info(env, info, &argc, argv, &this_arg, nullptr) != napi_ok || this_arg == nullptr) {
    return nullptr;
  }

  napi_value code = nullptr;
  if (argc >= 1 && argv[0] != nullptr) {
    napi_coerce_to_string(env, argv[0], &code);
  }
  if (code == nullptr) {
    napi_create_string_utf8(env, "", NAPI_AUTO_LENGTH, &code);
  }

  napi_value filename = nullptr;
  if (argc >= 2 && argv[1] != nullptr) {
    napi_coerce_to_string(env, argv[1], &filename);
  }
  if (filename == nullptr) {
    napi_create_string_utf8(env, "[eval]", NAPI_AUTO_LENGTH, &filename);
  }

  napi_set_named_property(env, this_arg, "__ubi_code", code);
  napi_set_named_property(env, this_arg, "__ubi_filename", filename);
  napi_set_named_property(env, this_arg, "sourceURL", filename);
  return this_arg;
}

static napi_value ContextifyScriptRunInContextCallback(napi_env env, napi_callback_info info) {
  napi_value this_arg = nullptr;
  size_t argc = 0;
  if (napi_get_cb_info(env, info, &argc, nullptr, &this_arg, nullptr) != napi_ok || this_arg == nullptr) {
    return nullptr;
  }

  napi_value code_value = nullptr;
  if (napi_get_named_property(env, this_arg, "__ubi_code", &code_value) != napi_ok || code_value == nullptr) {
    napi_get_undefined(env, &code_value);
  }
  napi_value filename_value = nullptr;
  if (napi_get_named_property(env, this_arg, "__ubi_filename", &filename_value) != napi_ok ||
      filename_value == nullptr) {
    napi_get_undefined(env, &filename_value);
  }

  const std::string code = ValueToUtf8(env, code_value);
  const std::string filename = ValueToUtf8(env, filename_value);
  std::string source = code;
  if (!filename.empty()) {
    source.append("\n//# sourceURL=");
    source.append(filename);
  }

  napi_value script = nullptr;
  if (napi_create_string_utf8(env, source.c_str(), source.size(), &script) != napi_ok || script == nullptr) {
    return nullptr;
  }
  napi_value result = nullptr;
  if (napi_run_script(env, script, &result) != napi_ok) {
    return nullptr;
  }
  return result;
}

static napi_value ContextifyCompileFunctionCallback(napi_env env, napi_callback_info info) {
  size_t argc = 10;
  napi_value argv[10] = {nullptr};
  if (napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr) != napi_ok) {
    return nullptr;
  }

  napi_value code = nullptr;
  if (argc >= 1 && argv[0] != nullptr) {
    napi_coerce_to_string(env, argv[0], &code);
  }
  if (code == nullptr) {
    napi_create_string_utf8(env, "", NAPI_AUTO_LENGTH, &code);
  }

  napi_value params = nullptr;
  if (argc >= 9 && argv[8] != nullptr) {
    params = argv[8];
  } else {
    napi_create_array_with_length(env, 0, &params);
  }

  static constexpr const char* kFactorySource =
      "(function(__code,__params){"
      "  const p = Array.isArray(__params) ? __params : [];"
      "  return Function(...p, __code);"
      "})";
  napi_value factory_script = nullptr;
  if (napi_create_string_utf8(env, kFactorySource, NAPI_AUTO_LENGTH, &factory_script) != napi_ok ||
      factory_script == nullptr) {
    return nullptr;
  }
  napi_value factory = nullptr;
  if (napi_run_script(env, factory_script, &factory) != napi_ok || factory == nullptr) {
    return nullptr;
  }
  napi_value global = nullptr;
  if (napi_get_global(env, &global) != napi_ok || global == nullptr) {
    return nullptr;
  }
  napi_value fn = nullptr;
  napi_value call_argv[2] = {code, params};
  if (napi_call_function(env, global, factory, 2, call_argv, &fn) != napi_ok || fn == nullptr) {
    return nullptr;
  }

  napi_value out = nullptr;
  if (napi_create_object(env, &out) != napi_ok || out == nullptr) return nullptr;
  napi_set_named_property(env, out, "function", fn);
  napi_value cached_data_produced = nullptr;
  napi_get_boolean(env, false, &cached_data_produced);
  napi_set_named_property(env, out, "cachedDataProduced", cached_data_produced);
  return out;
}

static napi_value ContextifyCompileFunctionForCJSLoaderCallback(napi_env env, napi_callback_info info) {
  size_t argc = 4;
  napi_value argv[4] = {nullptr};
  if (napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr) != napi_ok) {
    return nullptr;
  }
  napi_value content_value = nullptr;
  if (argc >= 1 && argv[0] != nullptr) {
    napi_coerce_to_string(env, argv[0], &content_value);
  }
  if (content_value == nullptr) {
    napi_create_string_utf8(env, "", NAPI_AUTO_LENGTH, &content_value);
  }
  const std::string content = ValueToUtf8(env, content_value);
  const std::string wrapped =
      "(function (exports, require, module, __filename, __dirname) { " + content + "\n});";
  napi_value wrapped_script = nullptr;
  if (napi_create_string_utf8(env, wrapped.c_str(), wrapped.size(), &wrapped_script) != napi_ok ||
      wrapped_script == nullptr) {
    return nullptr;
  }
  napi_value compiled_wrapper = nullptr;
  if (napi_run_script(env, wrapped_script, &compiled_wrapper) != napi_ok || compiled_wrapper == nullptr) {
    return nullptr;
  }
  napi_value out = nullptr;
  if (napi_create_object(env, &out) != napi_ok || out == nullptr) return nullptr;
  napi_set_named_property(env, out, "function", compiled_wrapper);
  napi_value can_parse_as_esm = nullptr;
  napi_get_boolean(env, false, &can_parse_as_esm);
  napi_set_named_property(env, out, "canParseAsESM", can_parse_as_esm);
  return out;
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

  napi_value config_json = nullptr;
  const std::string builtins_config_json = LoadBuiltinsConfigJson();
  if (napi_create_string_utf8(env, builtins_config_json.c_str(), NAPI_AUTO_LENGTH, &config_json) != napi_ok ||
      config_json == nullptr ||
      napi_set_named_property(env, binding, "config", config_json) != napi_ok) {
    return nullptr;
  }

  napi_value has_cached_builtins_fn = nullptr;
  if (napi_create_function(env, "hasCachedBuiltins", NAPI_AUTO_LENGTH, ReturnTrueCallback, nullptr,
                           &has_cached_builtins_fn) != napi_ok ||
      has_cached_builtins_fn == nullptr ||
      napi_set_named_property(env, binding, "hasCachedBuiltins", has_cached_builtins_fn) != napi_ok) {
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
    napi_value binding = UbiGetOrCreateErrorsBinding(env);
    return binding != nullptr ? binding : undefined;
  }
  if (name == "trace_events") {
    napi_value binding = GetOrCreateTraceEventsBinding(env);
    return binding != nullptr ? binding : undefined;
  }
  if (name == "credentials") {
    napi_value out = nullptr;
    if (napi_create_object(env, &out) != napi_ok || out == nullptr) return undefined;
    napi_value f = nullptr;
    napi_get_boolean(env, false, &f);
    napi_set_named_property(env, out, "implementsPosixCredentials", f);
    return out;
  }
  if (name == "async_wrap") {
    napi_value out = nullptr;
    if (napi_create_object(env, &out) != napi_ok || out == nullptr) return undefined;
    napi_value setup_hooks = nullptr;
    if (napi_create_function(env, "setupHooks", NAPI_AUTO_LENGTH, NoopCallback, nullptr, &setup_hooks) == napi_ok &&
        setup_hooks != nullptr) {
      napi_set_named_property(env, out, "setupHooks", setup_hooks);
    }
    return out;
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
    napi_set_named_property(env, out, "openSSLIsBoringSSL", f);
    napi_set_named_property(env, out, "fipsMode", f);
    napi_set_named_property(env, out, "hasNodeOptions", t);
    napi_set_named_property(env, out, "noBrowserGlobals", f);
    napi_set_named_property(env, out, "isDebugBuild", f);
    return out;
  }
  if (name == "contextify") {
    napi_value global = nullptr;
    if (napi_get_global(env, &global) == napi_ok && global != nullptr) {
      bool has_binding = false;
      if (napi_has_named_property(env, global, "__ubi_contextify_binding", &has_binding) == napi_ok &&
          has_binding) {
        napi_value existing = nullptr;
        if (napi_get_named_property(env, global, "__ubi_contextify_binding", &existing) == napi_ok &&
            existing != nullptr) {
          return existing;
        }
      }
    }

    napi_value out = nullptr;
    if (napi_create_object(env, &out) != napi_ok || out == nullptr) return undefined;
    napi_value contains_module_syntax = nullptr;
    if (napi_create_function(env,
                             "containsModuleSyntax",
                             NAPI_AUTO_LENGTH,
                             ReturnFalseCallback,
                             nullptr,
                             &contains_module_syntax) == napi_ok &&
        contains_module_syntax != nullptr) {
      napi_set_named_property(env, out, "containsModuleSyntax", contains_module_syntax);
    }

    napi_value contextify_script_ctor = nullptr;
    if (napi_create_function(env,
                             "ContextifyScript",
                             NAPI_AUTO_LENGTH,
                             ContextifyScriptConstructorCallback,
                             nullptr,
                             &contextify_script_ctor) == napi_ok &&
        contextify_script_ctor != nullptr) {
      napi_value proto = nullptr;
      if (napi_get_named_property(env, contextify_script_ctor, "prototype", &proto) == napi_ok && proto != nullptr) {
        napi_value run_in_context = nullptr;
        if (napi_create_function(env,
                                 "runInContext",
                                 NAPI_AUTO_LENGTH,
                                 ContextifyScriptRunInContextCallback,
                                 nullptr,
                                 &run_in_context) == napi_ok &&
            run_in_context != nullptr) {
          napi_set_named_property(env, proto, "runInContext", run_in_context);
        }
      }
      napi_set_named_property(env, out, "ContextifyScript", contextify_script_ctor);
    }

    napi_value compile_function = nullptr;
    if (napi_create_function(env,
                             "compileFunction",
                             NAPI_AUTO_LENGTH,
                             ContextifyCompileFunctionCallback,
                             nullptr,
                             &compile_function) == napi_ok &&
        compile_function != nullptr) {
      napi_set_named_property(env, out, "compileFunction", compile_function);
    }

    napi_value compile_function_for_cjs = nullptr;
    if (napi_create_function(env,
                             "compileFunctionForCJSLoader",
                             NAPI_AUTO_LENGTH,
                             ContextifyCompileFunctionForCJSLoaderCallback,
                             nullptr,
                             &compile_function_for_cjs) == napi_ok &&
        compile_function_for_cjs != nullptr) {
      napi_set_named_property(env, out, "compileFunctionForCJSLoader", compile_function_for_cjs);
    }

    if (global != nullptr) {
      napi_set_named_property(env, global, "__ubi_contextify_binding", out);
    }
    return out;
  }
  if (name == "symbols") {
    napi_value global = nullptr;
    if (napi_get_global(env, &global) != napi_ok || global == nullptr) {
      return undefined;
    }
    bool has_binding = false;
    if (napi_has_named_property(env, global, "__ubi_symbols_binding", &has_binding) == napi_ok &&
        has_binding) {
      napi_value existing = nullptr;
      if (napi_get_named_property(env, global, "__ubi_symbols_binding", &existing) == napi_ok &&
          existing != nullptr) {
        return existing;
      }
    }

    napi_value out = nullptr;
    if (napi_create_object(env, &out) != napi_ok || out == nullptr) return undefined;
    auto set_symbol = [&](const char* key, const char* description) {
      napi_value desc = nullptr;
      napi_value sym = nullptr;
      if (napi_create_string_utf8(env, description, NAPI_AUTO_LENGTH, &desc) == napi_ok &&
          desc != nullptr &&
          napi_create_symbol(env, desc, &sym) == napi_ok &&
          sym != nullptr) {
        napi_set_named_property(env, out, key, sym);
      }
    };

    set_symbol("vm_dynamic_import_default_internal", "vm_dynamic_import_default_internal");
    set_symbol("vm_dynamic_import_main_context_default", "vm_dynamic_import_main_context_default");
    set_symbol("vm_dynamic_import_no_callback", "vm_dynamic_import_no_callback");
    set_symbol("vm_dynamic_import_missing_flag", "vm_dynamic_import_missing_flag");
    set_symbol("source_text_module_default_hdo", "source_text_module_default_hdo");
    set_symbol("resource_symbol", "resource_symbol");
    set_symbol("owner_symbol", "owner_symbol");
    set_symbol("async_id_symbol", "async_id_symbol");
    set_symbol("trigger_async_id_symbol", "trigger_async_id_symbol");
    set_symbol("onpskexchange", "onpskexchange");
    set_symbol("messaging_transfer_list_symbol", "messaging_transfer_list_symbol");
    set_symbol("no_message_symbol", "no_message_symbol");
    set_symbol("imported_cjs_symbol", "imported_cjs_symbol");

    napi_set_named_property(env, global, "__ubi_symbols_binding", out);
    return out;
  }
  if (name == "module_wrap") {
    napi_value out = nullptr;
    if (napi_create_object(env, &out) != napi_ok || out == nullptr) return undefined;
    set_int32(out, "kSourcePhase", 0);
    set_int32(out, "kEvaluationPhase", 1);
    set_int32(out, "kEvaluated", 2);
    napi_value create_required_module_facade = nullptr;
    if (napi_create_function(env,
                             "createRequiredModuleFacade",
                             NAPI_AUTO_LENGTH,
                             ReturnFirstArgCallback,
                             nullptr,
                             &create_required_module_facade) == napi_ok &&
        create_required_module_facade != nullptr) {
      napi_set_named_property(env, out, "createRequiredModuleFacade", create_required_module_facade);
    }
    return out;
  }
  if (name == "modules") {
    napi_value out = nullptr;
    if (napi_create_object(env, &out) != napi_ok || out == nullptr) return undefined;

    auto define_method = [&](const char* method_name, napi_callback cb) -> bool {
      napi_value fn = nullptr;
      return napi_create_function(env, method_name, NAPI_AUTO_LENGTH, cb, nullptr, &fn) == napi_ok &&
             fn != nullptr &&
             napi_set_named_property(env, out, method_name, fn) == napi_ok;
    };

    if (!define_method("readPackageJSON", ModulesReadPackageJSONCallback) ||
        !define_method("getNearestParentPackageJSONType", ModulesGetNearestParentPackageJSONTypeCallback) ||
        !define_method("getNearestParentPackageJSON", ModulesGetNearestParentPackageJSONCallback) ||
        !define_method("getPackageScopeConfig", ModulesGetPackageScopeConfigCallback) ||
        !define_method("getPackageType", ModulesGetPackageTypeCallback) ||
        !define_method("enableCompileCache", ModulesEnableCompileCacheCallback) ||
        !define_method("getCompileCacheDir", ModulesGetCompileCacheDirCallback) ||
        !define_method("flushCompileCache", ModulesFlushCompileCacheCallback) ||
        !define_method("getCompileCacheEntry", ModulesGetCompileCacheEntryCallback) ||
        !define_method("saveCompileCacheEntry", ModulesSaveCompileCacheEntryCallback) ||
        !define_method("setLazyPathHelpers", ModulesSetLazyPathHelpersCallback)) {
      return undefined;
    }

    napi_value status = nullptr;
    if (napi_create_array_with_length(env, 4, &status) != napi_ok || status == nullptr) return undefined;
    auto set_status = [&](uint32_t idx, const char* value) {
      napi_value v = nullptr;
      if (napi_create_string_utf8(env, value, NAPI_AUTO_LENGTH, &v) == napi_ok && v != nullptr) {
        napi_set_element(env, status, idx, v);
      }
    };
    set_status(0, "FAILED");
    set_status(1, "ENABLED");
    set_status(2, "ALREADY_ENABLED");
    set_status(3, "DISABLED");
    if (napi_set_named_property(env, out, "compileCacheStatus", status) != napi_ok) return undefined;

    napi_value cached_types = nullptr;
    if (napi_create_object(env, &cached_types) != napi_ok || cached_types == nullptr) return undefined;
    set_int32(cached_types, "kCommonJS", 0);
    set_int32(cached_types, "kESM", 1);
    set_int32(cached_types, "kStrippedTypeScript", 2);
    set_int32(cached_types, "kTransformedTypeScript", 3);
    set_int32(cached_types, "kTransformedTypeScriptWithSourceMaps", 4);
    if (napi_set_named_property(env, out, "cachedCodeTypes", cached_types) != napi_ok) return undefined;

    return out;
  }
  if (name == "options") {
    napi_value out = nullptr;
    if (napi_create_object(env, &out) != napi_ok || out == nullptr) return undefined;

    napi_value env_settings = nullptr;
    if (napi_create_object(env, &env_settings) != napi_ok || env_settings == nullptr) return undefined;
    set_int32(env_settings, "kAllowedInEnvvar", 0);
    set_int32(env_settings, "kDisallowedInEnvvar", 1);
    if (napi_set_named_property(env, out, "envSettings", env_settings) != napi_ok) return undefined;

    napi_value types = nullptr;
    if (napi_create_object(env, &types) != napi_ok || types == nullptr) return undefined;
    set_int32(types, "kNoOp", 0);
    set_int32(types, "kV8Option", 1);
    set_int32(types, "kBoolean", 2);
    set_int32(types, "kInteger", 3);
    set_int32(types, "kUInteger", 4);
    set_int32(types, "kString", 5);
    set_int32(types, "kHostPort", 6);
    set_int32(types, "kStringList", 7);
    if (napi_set_named_property(env, out, "types", types) != napi_ok) return undefined;

    auto define_method = [&](const char* method_name, napi_callback cb) -> bool {
      napi_value fn = nullptr;
      return napi_create_function(env, method_name, NAPI_AUTO_LENGTH, cb, nullptr, &fn) == napi_ok &&
             fn != nullptr &&
             napi_set_named_property(env, out, method_name, fn) == napi_ok;
    };
    if (!define_method("getCLIOptionsValues", OptionsGetCLIOptionsValuesCallback) ||
        !define_method("getCLIOptionsInfo", OptionsGetCLIOptionsInfoCallback) ||
        !define_method("getOptionsAsFlags", OptionsGetOptionsAsFlagsCallback) ||
        !define_method("getEmbedderOptions", OptionsGetEmbedderOptionsCallback) ||
        !define_method("getEnvOptionsInputType", OptionsGetEnvOptionsInputTypeCallback) ||
        !define_method("getNamespaceOptionsInputType", OptionsGetNamespaceOptionsInputTypeCallback)) {
      return undefined;
    }

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

napi_value ResolveSpecifierForContext(napi_env env, RequireContext* context, const std::string& specifier, bool throw_on_error) {
  fs::path resolved_path;
  if (ResolveBuiltinPath(specifier, context->base_dir, &resolved_path) ||
      ResolveModulePath(specifier, context->base_dir, &resolved_path) ||
      ResolveNodeModules(specifier, context->base_dir, &resolved_path)) {
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
          const std::string path_for_msg = resolved_path.string();
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
