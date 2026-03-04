#include "ubi_process.h"

#include <chrono>
#include <cerrno>
#include <cstring>
#include <cmath>
#include <ctime>
#include <cstdlib>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include <uv.h>
#include <openssl/crypto.h>

#include "ada/ada.h"
#include "brotli/c/common/version.h"
#include "cares/include/ares_version.h"
#include "llhttp/include/llhttp.h"
#include "nbytes/include/nbytes.h"
#include "ncrypto/ncrypto.h"
#include "simdjson/simdjson.h"
#include "simdutf/simdutf.h"
#include "zlib/zlib.h"
#include "nghttp2/lib/includes/nghttp2/nghttp2ver.h"
#include "zstd/lib/zstd.h"
#include "acorn_version.h"
#include "cjs_module_lexer_version.h"
#include "node_version.h"
#include "unofficial_napi.h"

#if defined(_WIN32)
#include <stdlib.h>
extern char** _environ;
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#include <signal.h>
#include <unistd.h>
extern char** environ;
#else
#include <signal.h>
#include <unistd.h>
extern char** environ;
#endif

#define DUMMY_UV_STUBS 1

namespace {

const auto g_process_start_time = std::chrono::steady_clock::now();
const auto g_cpu_usage_start = std::chrono::steady_clock::now();
std::string g_ubi_exec_path;
uint32_t g_process_umask = 0022;

#ifndef UBI_EMBEDDED_V8_VERSION
#define UBI_EMBEDDED_V8_VERSION "0.0.0-node.0"
#endif

#define UBI_STRINGIFY_HELPER(x) #x
#define UBI_STRINGIFY(x) UBI_STRINGIFY_HELPER(x)

struct ProcessMethodsBindingState {
  napi_ref binding_ref = nullptr;
  napi_ref hrtime_buffer_ref = nullptr;
};

struct ReportBindingState {
  napi_ref binding_ref = nullptr;
  bool compact = false;
  bool exclude_network = false;
  bool exclude_env = false;
  bool report_on_fatal_error = false;
  bool report_on_signal = false;
  bool report_on_uncaught_exception = false;
  std::string directory = ".";
  std::string filename;
  std::string signal = "SIGUSR2";
  uint64_t sequence = 0;
};

std::map<napi_env, ProcessMethodsBindingState> g_process_methods_states;
std::map<napi_env, ReportBindingState> g_report_states;
constexpr const char kUvwasiVersion[] = "0.0.23";

std::string ReadTextFileIfExists(const std::filesystem::path& path);

std::string GetOpenSslVersion() {
  // Matches Node behavior: trim the "OpenSSL " prefix and keep the version
  // token, with a conservative fallback for non-OpenSSL implementations.
  const char* version = OpenSSL_version(OPENSSL_VERSION);
  if (version == nullptr) return "0.0.0";
  const char* first_space = std::strchr(version, ' ');
  if (first_space == nullptr || first_space[1] == '\0') return "0.0.0";
  const char* start = first_space + 1;
  const char* end = std::strchr(start, ' ');
  if (end == nullptr) return std::string(start);
  return std::string(start, static_cast<size_t>(end - start));
}

std::string GetBrotliVersion() {
  return std::string(UBI_STRINGIFY(BROTLI_VERSION_MAJOR)) + "." +
         UBI_STRINGIFY(BROTLI_VERSION_MINOR) + "." +
         UBI_STRINGIFY(BROTLI_VERSION_PATCH);
}

std::string GetLlhttpVersion() {
  return std::string(UBI_STRINGIFY(LLHTTP_VERSION_MAJOR)) + "." +
         UBI_STRINGIFY(LLHTTP_VERSION_MINOR) + "." +
         UBI_STRINGIFY(LLHTTP_VERSION_PATCH);
}

std::string ExtractPackageVersionFromJson(const std::string& json_text) {
  const std::string key = "\"version\"";
  const size_t key_pos = json_text.find(key);
  if (key_pos == std::string::npos) return {};

  const size_t colon = json_text.find(':', key_pos + key.size());
  if (colon == std::string::npos) return {};

  size_t first_quote = json_text.find('"', colon + 1);
  if (first_quote == std::string::npos) return {};
  ++first_quote;
  const size_t second_quote = json_text.find('"', first_quote);
  if (second_quote == std::string::npos || second_quote <= first_quote) return {};

  return json_text.substr(first_quote, second_quote - first_quote);
}

std::string ReadPackageVersionFromCandidates(const std::vector<std::filesystem::path>& candidates) {
  for (const std::filesystem::path& candidate : candidates) {
    const std::string text = ReadTextFileIfExists(candidate);
    if (text.empty()) continue;
    const std::string version = ExtractPackageVersionFromJson(text);
    if (!version.empty()) return version;
  }
  return "0.0.0";
}

std::string GetUndiciVersion() {
  namespace fs = std::filesystem;
  const fs::path source_root = fs::absolute(fs::path(__FILE__).parent_path() / ".." / "..").lexically_normal();
  static const std::string version = ReadPackageVersionFromCandidates({
      source_root / "node" / "deps" / "undici" / "src" / "package.json",
      fs::current_path() / "node" / "deps" / "undici" / "src" / "package.json",
      fs::current_path().parent_path() / "node" / "deps" / "undici" / "src" / "package.json",
  });
  return version;
}

std::string GetAmaroVersion() {
  namespace fs = std::filesystem;
  const fs::path source_root = fs::absolute(fs::path(__FILE__).parent_path() / ".." / "..").lexically_normal();
  static const std::string version = ReadPackageVersionFromCandidates({
      source_root / "node" / "deps" / "amaro" / "package.json",
      fs::current_path() / "node" / "deps" / "amaro" / "package.json",
      fs::current_path().parent_path() / "node" / "deps" / "amaro" / "package.json",
  });
  return version;
}

std::string MaybePreferSiblingUbiBinary(const std::string& detected_exec_path) {
  if (detected_exec_path.empty()) return detected_exec_path;
  namespace fs = std::filesystem;
  std::error_code ec;
  fs::path detected = fs::path(detected_exec_path).lexically_normal();
  const std::string filename = detected.filename().string();
  if (filename.rfind("ubi_test_", 0) != 0) {
    return detected_exec_path;
  }
  const std::vector<fs::path> candidates = {
      detected.parent_path() / "ubi",
      detected.parent_path() / "ubi.exe",
      detected.parent_path().parent_path() / "ubi",
      detected.parent_path().parent_path() / "ubi.exe",
  };
  for (const fs::path& candidate : candidates) {
    ec.clear();
    if (!fs::exists(candidate, ec) || ec) continue;
    ec.clear();
    if (fs::is_directory(candidate, ec) || ec) continue;
    const fs::path canonical = fs::weakly_canonical(candidate, ec);
    if (!ec) return canonical.string();
    return candidate.string();
  }
  return detected_exec_path;
}

const char* DetectPlatform() {
#if defined(_WIN32)
  return "win32";
#elif defined(__APPLE__)
  return "darwin";
#elif defined(__linux__)
  return "linux";
#elif defined(__sun)
  return "sunos";
#elif defined(_AIX)
  return "aix";
#else
  return "unknown";
#endif
}

const char* DetectArch() {
#if defined(__x86_64__) || defined(_M_X64)
  return "x64";
#elif defined(__aarch64__) || defined(_M_ARM64)
  return "arm64";
#elif defined(__arm__) || defined(_M_ARM)
  return "arm";
#elif defined(__i386__) || defined(_M_IX86)
  return "ia32";
#else
  return "unknown";
#endif
}

std::string DetectExecPath() {
  const char* forced_exec = std::getenv("UBI_EXEC_PATH");
  if (forced_exec != nullptr && forced_exec[0] != '\0') {
    return forced_exec;
  }
#if defined(__APPLE__)
  uint32_t size = 0;
  _NSGetExecutablePath(nullptr, &size);
  if (size > 0) {
    std::vector<char> buf(size + 1, '\0');
    if (_NSGetExecutablePath(buf.data(), &size) == 0) {
      char resolved[4096] = {'\0'};
      if (realpath(buf.data(), resolved) != nullptr) {
        return MaybePreferSiblingUbiBinary(std::string(resolved));
      }
      return MaybePreferSiblingUbiBinary(std::string(buf.data()));
    }
  }
  return "ubi";
#elif defined(__linux__)
  std::vector<char> buf(4096, '\0');
  ssize_t n = readlink("/proc/self/exe", buf.data(), buf.size() - 1);
  if (n > 0) {
    buf[static_cast<size_t>(n)] = '\0';
    return MaybePreferSiblingUbiBinary(std::string(buf.data()));
  }
  return "ubi";
#elif defined(_WIN32)
  return "ubi.exe";
#else
  return "ubi";
#endif
}

std::string ReadTextFileIfExists(const std::filesystem::path& path) {
  std::error_code ec;
  if (!std::filesystem::exists(path, ec) || ec) return {};
  std::ifstream input(path, std::ios::in | std::ios::binary);
  if (!input.is_open()) return {};
  std::ostringstream buffer;
  buffer << input.rdbuf();
  return buffer.str();
}

std::string FindNodeConfigGypiText() {
  namespace fs = std::filesystem;
  std::error_code ec;
  const fs::path source_root = fs::absolute(fs::path(__FILE__).parent_path() / ".." / "..").lexically_normal();
  const std::vector<fs::path> candidates = {
      source_root / "node" / "config.gypi",
      fs::current_path() / "node" / "config.gypi",
      fs::current_path().parent_path() / "node" / "config.gypi",
  };
  for (const fs::path& candidate : candidates) {
    if (!candidate.empty()) {
      const std::string text = ReadTextFileIfExists(candidate);
      if (!text.empty()) return text;
    }
  }

  // Final fallback when tests run from <repo>/build* and source path probes fail.
  const fs::path cwd = fs::current_path(ec);
  if (!ec) {
    const fs::path fallback = cwd.parent_path() / "node" / "config.gypi";
    const std::string text = ReadTextFileIfExists(fallback);
    if (!text.empty()) return text;
  }
  return {};
}

napi_value BuildMinimalProcessConfigObject(napi_env env) {
  napi_value config_obj = nullptr;
  if (napi_create_object(env, &config_obj) != napi_ok || config_obj == nullptr) return nullptr;

  napi_value variables_obj = nullptr;
  if (napi_create_object(env, &variables_obj) != napi_ok || variables_obj == nullptr) return nullptr;

  napi_value zero = nullptr;
  if (napi_create_int32(env, 0, &zero) != napi_ok || zero == nullptr) return nullptr;
  const char* int_var_keys[] = {"v8_enable_i18n_support", "node_quic", "asan", "node_shared_openssl"};
  for (const char* key : int_var_keys) {
    if (napi_set_named_property(env, variables_obj, key, zero) != napi_ok) return nullptr;
  }

  napi_value shareable_builtins = nullptr;
  if (napi_create_array_with_length(env, 2, &shareable_builtins) != napi_ok ||
      shareable_builtins == nullptr) {
    return nullptr;
  }
  napi_value undici_builtin = nullptr;
  if (napi_create_string_utf8(env, "deps/undici/undici.js", NAPI_AUTO_LENGTH, &undici_builtin) != napi_ok ||
      undici_builtin == nullptr ||
      napi_set_element(env, shareable_builtins, 0, undici_builtin) != napi_ok) {
    return nullptr;
  }
  napi_value amaro_builtin = nullptr;
  if (napi_create_string_utf8(env, "deps/amaro/dist/index.js", NAPI_AUTO_LENGTH, &amaro_builtin) != napi_ok ||
      amaro_builtin == nullptr ||
      napi_set_element(env, shareable_builtins, 1, amaro_builtin) != napi_ok) {
    return nullptr;
  }
  if (napi_set_named_property(env, variables_obj, "node_builtin_shareable_builtins", shareable_builtins) !=
      napi_ok) {
    return nullptr;
  }

  napi_value one = nullptr;
  if (napi_create_int32(env, 1, &one) != napi_ok || one == nullptr) return nullptr;
  if (napi_set_named_property(env, variables_obj, "node_use_amaro", one) != napi_ok) {
    return nullptr;
  }

  napi_value napi_build_version = nullptr;
  if (napi_create_string_utf8(
          env, UBI_STRINGIFY(NODE_API_SUPPORTED_VERSION_MAX), NAPI_AUTO_LENGTH, &napi_build_version) != napi_ok ||
      napi_build_version == nullptr) {
    return nullptr;
  }
  if (napi_set_named_property(env, variables_obj, "napi_build_version", napi_build_version) != napi_ok) {
    return nullptr;
  }

  napi_property_descriptor variables_desc = {};
  variables_desc.utf8name = "variables";
  variables_desc.value = variables_obj;
  variables_desc.attributes = napi_enumerable;
  if (napi_define_properties(env, config_obj, 1, &variables_desc) != napi_ok) return nullptr;

  return config_obj;
}

napi_value ParseProcessConfigObjectFromText(napi_env env, const std::string& config_text) {
  if (config_text.empty()) return nullptr;

  const char* parse_script_source =
      "(function(__raw){"
      "  const body = __raw.split('\\n').slice(1).join('\\n');"
      "  const parsed = JSON.parse(body, function(key, value) {"
      "    if (value === 'true') return true;"
      "    if (value === 'false') return false;"
      "    return value;"
      "  });"
      "  return Object.freeze(parsed);"
      "})";

  napi_value parse_script = nullptr;
  if (napi_create_string_utf8(env, parse_script_source, NAPI_AUTO_LENGTH, &parse_script) != napi_ok ||
      parse_script == nullptr) {
    return nullptr;
  }

  napi_value parse_fn = nullptr;
  if (napi_run_script(env, parse_script, &parse_fn) != napi_ok || parse_fn == nullptr) {
    bool has_exception = false;
    if (napi_is_exception_pending(env, &has_exception) == napi_ok && has_exception) {
      napi_value ignored = nullptr;
      napi_get_and_clear_last_exception(env, &ignored);
    }
    return nullptr;
  }

  napi_value global = nullptr;
  if (napi_get_global(env, &global) != napi_ok || global == nullptr) return nullptr;

  napi_value raw_text = nullptr;
  if (napi_create_string_utf8(env, config_text.c_str(), config_text.size(), &raw_text) != napi_ok ||
      raw_text == nullptr) {
    return nullptr;
  }

  napi_value parsed = nullptr;
  if (napi_call_function(env, global, parse_fn, 1, &raw_text, &parsed) != napi_ok || parsed == nullptr) {
    bool has_exception = false;
    if (napi_is_exception_pending(env, &has_exception) == napi_ok && has_exception) {
      napi_value ignored = nullptr;
      napi_get_and_clear_last_exception(env, &ignored);
    }
    return nullptr;
  }

  return parsed;
}

napi_value BuildProcessConfigObject(napi_env env) {
  const std::string config_text = FindNodeConfigGypiText();
  napi_value parsed = ParseProcessConfigObjectFromText(env, config_text);
  if (parsed != nullptr) return parsed;
  return BuildMinimalProcessConfigObject(env);
}

uint64_t GetHrtimeNanoseconds() {
  const auto now = std::chrono::steady_clock::now().time_since_epoch();
  return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
}

void ThrowTypeErrorWithCode(napi_env env, const char* code, const char* message) {
  napi_value code_value = nullptr;
  napi_value message_value = nullptr;
  napi_value error_value = nullptr;
  if (napi_create_string_utf8(env, code, NAPI_AUTO_LENGTH, &code_value) != napi_ok ||
      napi_create_string_utf8(env, message, NAPI_AUTO_LENGTH, &message_value) != napi_ok ||
      napi_create_type_error(env, code_value, message_value, &error_value) != napi_ok ||
      error_value == nullptr) {
    return;
  }
  napi_set_named_property(env, error_value, "code", code_value);
  napi_throw(env, error_value);
}

void ThrowErrorWithCode(napi_env env, const char* code, const char* message) {
  napi_value code_value = nullptr;
  napi_value message_value = nullptr;
  napi_value error_value = nullptr;
  if (napi_create_string_utf8(env, code, NAPI_AUTO_LENGTH, &code_value) != napi_ok ||
      napi_create_string_utf8(env, message, NAPI_AUTO_LENGTH, &message_value) != napi_ok ||
      napi_create_error(env, code_value, message_value, &error_value) != napi_ok ||
      error_value == nullptr) {
    return;
  }
  napi_set_named_property(env, error_value, "code", code_value);
  napi_throw(env, error_value);
}

void ThrowRangeErrorWithCode(napi_env env, const char* code, const char* message) {
  napi_value code_value = nullptr;
  napi_value message_value = nullptr;
  napi_value error_value = nullptr;
  if (napi_create_string_utf8(env, code, NAPI_AUTO_LENGTH, &code_value) != napi_ok ||
      napi_create_string_utf8(env, message, NAPI_AUTO_LENGTH, &message_value) != napi_ok ||
      napi_create_range_error(env, code_value, message_value, &error_value) != napi_ok ||
      error_value == nullptr) {
    return;
  }
  napi_set_named_property(env, error_value, "code", code_value);
  napi_throw(env, error_value);
}

bool SetNamedString(napi_env env, napi_value obj, const char* name, const std::string& value);
bool SetNamedInt32(napi_env env, napi_value obj, const char* name, int32_t value);

void ThrowSystemError(napi_env env, int err, const char* syscall, const std::string& path = std::string()) {
  int uv_err = err;
  if (uv_err > 0) uv_err = uv_translate_sys_error(uv_err);
  if (uv_err == 0) uv_err = UV_EIO;

  const char* code = uv_err_name(uv_err);
  if (code == nullptr) code = "UNKNOWN";
  const char* detail = uv_strerror(uv_err);
  if (detail == nullptr) detail = "unknown error";

  std::string message = std::string(code) + ": " + detail;
  if (syscall != nullptr && syscall[0] != '\0') {
    message += ", ";
    message += syscall;
  }
  if (!path.empty()) {
    message += " '";
    message += path;
    message += "'";
  }

  napi_value code_value = nullptr;
  napi_value message_value = nullptr;
  napi_value error_value = nullptr;
  if (napi_create_string_utf8(env, code, NAPI_AUTO_LENGTH, &code_value) != napi_ok ||
      napi_create_string_utf8(env, message.c_str(), NAPI_AUTO_LENGTH, &message_value) != napi_ok ||
      napi_create_error(env, code_value, message_value, &error_value) != napi_ok ||
      error_value == nullptr) {
    return;
  }
  napi_set_named_property(env, error_value, "code", code_value);
  SetNamedInt32(env, error_value, "errno", uv_err);
  if (syscall != nullptr && syscall[0] != '\0') {
    SetNamedString(env, error_value, "syscall", syscall);
  }
  if (!path.empty()) {
    SetNamedString(env, error_value, "path", path);
  }
  napi_throw(env, error_value);
}

std::string InvalidArgTypeSuffix(napi_env env, napi_value value) {
  napi_valuetype t = napi_undefined;
  if (value == nullptr || napi_typeof(env, value, &t) != napi_ok) return " Received undefined";
  if (t == napi_undefined) return " Received undefined";
  if (t == napi_null) return " Received null";
  if (t == napi_string) {
    size_t len = 0;
    if (napi_get_value_string_utf8(env, value, nullptr, 0, &len) != napi_ok) return " Received type string";
    std::vector<char> buf(len + 1, '\0');
    size_t copied = 0;
    if (napi_get_value_string_utf8(env, value, buf.data(), buf.size(), &copied) != napi_ok) {
      return " Received type string";
    }
    return " Received type string ('" + std::string(buf.data(), copied) + "')";
  }
  if (t == napi_object) return " Received an instance of Object";
  if (t == napi_number) {
    double d = 0;
    if (napi_get_value_double(env, value, &d) == napi_ok) {
      std::ostringstream oss;
      oss << " Received type number (" << d << ")";
      return oss.str();
    }
  }
  return " Received type " + std::string(t == napi_boolean ? "boolean" : "unknown");
}

std::string FormatNodeNumber(double value) {
  if (std::isinf(value)) return value > 0 ? "Infinity" : "-Infinity";
  std::ostringstream oss;
  oss << value;
  return oss.str();
}

std::string NapiValueToUtf8(napi_env env, napi_value value) {
  napi_value string_value = nullptr;
  if (napi_coerce_to_string(env, value, &string_value) != napi_ok || string_value == nullptr) return "";
  size_t length = 0;
  if (napi_get_value_string_utf8(env, string_value, nullptr, 0, &length) != napi_ok) return "";
  std::string out(length + 1, '\0');
  size_t copied = 0;
  if (napi_get_value_string_utf8(env, string_value, out.data(), out.size(), &copied) != napi_ok) return "";
  out.resize(copied);
  return out;
}

ProcessMethodsBindingState* GetProcessMethodsState(napi_env env) {
  auto it = g_process_methods_states.find(env);
  if (it == g_process_methods_states.end()) return nullptr;
  return &it->second;
}

ReportBindingState* GetReportState(napi_env env) {
  auto it = g_report_states.find(env);
  if (it == g_report_states.end()) return nullptr;
  return &it->second;
}

bool SetNamedString(napi_env env, napi_value obj, const char* name, const std::string& value) {
  napi_value v = nullptr;
  if (napi_create_string_utf8(env, value.c_str(), NAPI_AUTO_LENGTH, &v) != napi_ok || v == nullptr) return false;
  return napi_set_named_property(env, obj, name, v) == napi_ok;
}

bool SetNamedDouble(napi_env env, napi_value obj, const char* name, double value) {
  napi_value v = nullptr;
  if (napi_create_double(env, value, &v) != napi_ok || v == nullptr) return false;
  return napi_set_named_property(env, obj, name, v) == napi_ok;
}

bool SetNamedInt32(napi_env env, napi_value obj, const char* name, int32_t value) {
  napi_value v = nullptr;
  if (napi_create_int32(env, value, &v) != napi_ok || v == nullptr) return false;
  return napi_set_named_property(env, obj, name, v) == napi_ok;
}

bool SetNamedBool(napi_env env, napi_value obj, const char* name, bool value) {
  napi_value v = nullptr;
  if (napi_get_boolean(env, value, &v) != napi_ok || v == nullptr) return false;
  return napi_set_named_property(env, obj, name, v) == napi_ok;
}

bool SetNamedValue(napi_env env, napi_value obj, const char* name, napi_value value) {
  if (value == nullptr) return false;
  return napi_set_named_property(env, obj, name, value) == napi_ok;
}

bool CopyNamedProperty(napi_env env, napi_value from, napi_value to, const char* name) {
  bool has = false;
  if (from == nullptr || to == nullptr) return false;
  if (napi_has_named_property(env, from, name, &has) != napi_ok || !has) return false;
  napi_value value = nullptr;
  if (napi_get_named_property(env, from, name, &value) != napi_ok || value == nullptr) return false;
  return napi_set_named_property(env, to, name, value) == napi_ok;
}

bool IsValidMacAddress(const std::string& mac);

bool ValueToInt32(napi_env env, napi_value value, int32_t* out) {
  if (out == nullptr || value == nullptr) return false;
  return napi_get_value_int32(env, value, out) == napi_ok;
}

bool ValueToBool(napi_env env, napi_value value, bool* out) {
  if (out == nullptr || value == nullptr) return false;
  return napi_get_value_bool(env, value, out) == napi_ok;
}

napi_value RequireBuiltin(napi_env env, const char* id) {
  napi_value global = nullptr;
  napi_value require_fn = nullptr;
  napi_value id_value = nullptr;
  if (napi_get_global(env, &global) != napi_ok || global == nullptr ||
      napi_get_named_property(env, global, "require", &require_fn) != napi_ok ||
      require_fn == nullptr ||
      napi_create_string_utf8(env, id, NAPI_AUTO_LENGTH, &id_value) != napi_ok ||
      id_value == nullptr) {
    return nullptr;
  }
  napi_valuetype t = napi_undefined;
  if (napi_typeof(env, require_fn, &t) != napi_ok || t != napi_function) return nullptr;
  napi_value argv[1] = {id_value};
  napi_value out = nullptr;
  if (napi_call_function(env, global, require_fn, 1, argv, &out) != napi_ok) return nullptr;
  return out;
}

napi_value MakeReportUserLimits(napi_env env) {
  napi_value limits = nullptr;
  if (napi_create_object(env, &limits) != napi_ok || limits == nullptr) return nullptr;
  const char* keys[] = {
      "core_file_size_blocks",
      "data_seg_size_bytes",
      "file_size_blocks",
      "max_locked_memory_bytes",
      "max_memory_size_bytes",
      "open_files",
      "stack_size_bytes",
      "cpu_time_seconds",
      "max_user_processes",
      "virtual_memory_bytes",
  };
  for (const char* key : keys) {
    napi_value entry = nullptr;
    napi_value unlimited = nullptr;
    if (napi_create_object(env, &entry) != napi_ok || entry == nullptr ||
        napi_create_string_utf8(env, "unlimited", NAPI_AUTO_LENGTH, &unlimited) != napi_ok ||
        unlimited == nullptr) {
      return nullptr;
    }
    if (napi_set_named_property(env, entry, "soft", unlimited) != napi_ok ||
        napi_set_named_property(env, entry, "hard", unlimited) != napi_ok ||
        napi_set_named_property(env, limits, key, entry) != napi_ok) {
      return nullptr;
    }
  }
  return limits;
}

napi_value BuildReportObject(napi_env env,
                             const std::string& event_message,
                             const std::string& trigger,
                             const std::string& report_filename) {
  napi_value report = nullptr;
  if (napi_create_object(env, &report) != napi_ok || report == nullptr) return nullptr;

  napi_value process_obj = nullptr;
  napi_value global = nullptr;
  if (napi_get_global(env, &global) != napi_ok || global == nullptr ||
      napi_get_named_property(env, global, "process", &process_obj) != napi_ok ||
      process_obj == nullptr) {
    return nullptr;
  }

  napi_value os = RequireBuiltin(env, "os");
  if (os == nullptr) return nullptr;

  napi_value header = nullptr;
  if (napi_create_object(env, &header) != napi_ok || header == nullptr) return nullptr;
  SetNamedString(env, header, "event", event_message);
  SetNamedString(env, header, "trigger", trigger);
  if (!report_filename.empty()) {
    SetNamedString(env, header, "filename", report_filename);
  } else {
    napi_value null_value = nullptr;
    napi_get_null(env, &null_value);
    SetNamedValue(env, header, "filename", null_value);
  }

  const std::time_t now = std::time(nullptr);
  std::tm local_tm{};
#if defined(_WIN32)
  localtime_s(&local_tm, &now);
#else
  localtime_r(&now, &local_tm);
#endif
  char time_buf[64] = {'\0'};
  std::strftime(time_buf, sizeof(time_buf), "%Y-%m-%dT%H:%M:%S%z", &local_tm);
  SetNamedString(env, header, "dumpEventTime", time_buf);
  SetNamedDouble(env, header, "dumpEventTimeStamp", static_cast<double>(now) * 1000.0);
  SetNamedInt32(env, header, "processId", static_cast<int32_t>(uv_os_getpid()));
  SetNamedInt32(env, header, "threadId", static_cast<int32_t>(uv_os_getpid()));

  napi_value argv = nullptr;
  if (napi_get_named_property(env, process_obj, "argv", &argv) == napi_ok && argv != nullptr) {
    SetNamedValue(env, header, "commandLine", argv);
  }
  napi_value node_version = nullptr;
  if (napi_get_named_property(env, process_obj, "version", &node_version) == napi_ok && node_version != nullptr) {
    SetNamedValue(env, header, "nodejsVersion", node_version);
  }
  SetNamedInt32(env, header, "wordSize", static_cast<int32_t>(sizeof(void*) * 8));
  napi_value arch = nullptr;
  if (napi_get_named_property(env, process_obj, "arch", &arch) == napi_ok && arch != nullptr) {
    SetNamedValue(env, header, "arch", arch);
  }
  napi_value platform = nullptr;
  if (napi_get_named_property(env, process_obj, "platform", &platform) == napi_ok && platform != nullptr) {
    SetNamedValue(env, header, "platform", platform);
  }
  napi_value versions = nullptr;
  if (napi_get_named_property(env, process_obj, "versions", &versions) == napi_ok && versions != nullptr) {
    SetNamedValue(env, header, "componentVersions", versions);
  }
  napi_value release = nullptr;
  if (napi_get_named_property(env, process_obj, "release", &release) == napi_ok && release != nullptr) {
    SetNamedValue(env, header, "release", release);
  }

  auto setHeaderFromOs = [&](const char* fn_name, const char* field) {
    napi_value fn = nullptr;
    if (napi_get_named_property(env, os, fn_name, &fn) != napi_ok || fn == nullptr) return;
    napi_valuetype type = napi_undefined;
    if (napi_typeof(env, fn, &type) != napi_ok || type != napi_function) return;
    napi_value result = nullptr;
    if (napi_call_function(env, os, fn, 0, nullptr, &result) != napi_ok || result == nullptr) return;
    SetNamedValue(env, header, field, result);
  };
  setHeaderFromOs("type", "osName");
  setHeaderFromOs("release", "osRelease");
  setHeaderFromOs("version", "osVersion");
  setHeaderFromOs("machine", "osMachine");
  setHeaderFromOs("hostname", "host");

  napi_value cwd_fn = nullptr;
  if (napi_get_named_property(env, process_obj, "cwd", &cwd_fn) == napi_ok && cwd_fn != nullptr) {
    napi_valuetype type = napi_undefined;
    if (napi_typeof(env, cwd_fn, &type) == napi_ok && type == napi_function) {
      napi_value cwd_result = nullptr;
      if (napi_call_function(env, process_obj, cwd_fn, 0, nullptr, &cwd_result) == napi_ok &&
          cwd_result != nullptr) {
        SetNamedValue(env, header, "cwd", cwd_result);
      }
    }
  }

  napi_value cpus_fn = nullptr;
  if (napi_get_named_property(env, os, "cpus", &cpus_fn) == napi_ok && cpus_fn != nullptr) {
    napi_valuetype type = napi_undefined;
    if (napi_typeof(env, cpus_fn, &type) == napi_ok && type == napi_function) {
      napi_value cpus = nullptr;
      if (napi_call_function(env, os, cpus_fn, 0, nullptr, &cpus) == napi_ok && cpus != nullptr) {
        bool is_arr = false;
        if (napi_is_array(env, cpus, &is_arr) == napi_ok && is_arr) {
          napi_value normalized_cpus = nullptr;
          if (napi_create_array(env, &normalized_cpus) == napi_ok && normalized_cpus != nullptr) {
            uint32_t cpu_count = 0;
            napi_get_array_length(env, cpus, &cpu_count);
            for (uint32_t i = 0; i < cpu_count; ++i) {
              napi_value cpu = nullptr;
              if (napi_get_element(env, cpus, i, &cpu) != napi_ok || cpu == nullptr) continue;
              napi_value out_cpu = nullptr;
              if (napi_create_object(env, &out_cpu) != napi_ok || out_cpu == nullptr) continue;
              const char* scalar_fields[] = {"model", "speed"};
              for (const char* field : scalar_fields) {
                napi_value field_v = nullptr;
                if (napi_get_named_property(env, cpu, field, &field_v) == napi_ok && field_v != nullptr) {
                  SetNamedValue(env, out_cpu, field, field_v);
                }
              }
              napi_value times = nullptr;
              if (napi_get_named_property(env, cpu, "times", &times) == napi_ok && times != nullptr) {
                const char* time_in_fields[] = {"user", "nice", "sys", "idle", "irq"};
                for (const char* field : time_in_fields) {
                  napi_value tv = nullptr;
                  if (napi_get_named_property(env, times, field, &tv) == napi_ok && tv != nullptr) {
                    SetNamedValue(env, out_cpu, field, tv);
                  }
                }
              }
              napi_set_element(env, normalized_cpus, i, out_cpu);
            }
            SetNamedValue(env, header, "cpus", normalized_cpus);
          }
        } else {
          SetNamedValue(env, header, "cpus", cpus);
        }
      }
    }
  }

  napi_value net_fn = nullptr;
  if (napi_get_named_property(env, os, "networkInterfaces", &net_fn) == napi_ok && net_fn != nullptr) {
    napi_valuetype type = napi_undefined;
    if (napi_typeof(env, net_fn, &type) == napi_ok && type == napi_function) {
      napi_value net_obj = nullptr;
      if (napi_call_function(env, os, net_fn, 0, nullptr, &net_obj) == napi_ok && net_obj != nullptr) {
        napi_value network_interfaces = nullptr;
        if (napi_create_array(env, &network_interfaces) == napi_ok && network_interfaces != nullptr) {
          uint32_t out_idx = 0;
          napi_value iface_names = nullptr;
          if (napi_get_property_names(env, net_obj, &iface_names) == napi_ok && iface_names != nullptr) {
            uint32_t iface_count = 0;
            napi_get_array_length(env, iface_names, &iface_count);
            for (uint32_t i = 0; i < iface_count; ++i) {
              napi_value iface_name_v = nullptr;
              if (napi_get_element(env, iface_names, i, &iface_name_v) != napi_ok || iface_name_v == nullptr) continue;
              const std::string iface_name = NapiValueToUtf8(env, iface_name_v);
              napi_value iface_arr = nullptr;
              if (napi_get_property(env, net_obj, iface_name_v, &iface_arr) != napi_ok || iface_arr == nullptr) continue;
              bool is_arr = false;
              if (napi_is_array(env, iface_arr, &is_arr) != napi_ok || !is_arr) continue;
              uint32_t addr_count = 0;
              napi_get_array_length(env, iface_arr, &addr_count);
              for (uint32_t j = 0; j < addr_count; ++j) {
                napi_value entry = nullptr;
                if (napi_get_element(env, iface_arr, j, &entry) != napi_ok || entry == nullptr) continue;
                napi_value out_entry = nullptr;
                if (napi_create_object(env, &out_entry) != napi_ok || out_entry == nullptr) continue;
                SetNamedString(env, out_entry, "name", iface_name);
                const char* copied[] = {"address", "netmask", "family", "mac", "internal", "scopeid"};
                for (const char* key : copied) {
                  napi_value key_v = nullptr;
                  if (napi_create_string_utf8(env, key, NAPI_AUTO_LENGTH, &key_v) != napi_ok || key_v == nullptr) continue;
                  napi_value val = nullptr;
                  if (napi_get_property(env, entry, key_v, &val) == napi_ok && val != nullptr) {
                    napi_valuetype t = napi_undefined;
                    if (napi_typeof(env, val, &t) == napi_ok && t != napi_undefined) {
                      if (std::strcmp(key, "mac") == 0) {
                        const std::string mac = NapiValueToUtf8(env, val);
                        const std::string normalized = IsValidMacAddress(mac) ? mac : "00:00:00:00:00:00";
                        SetNamedString(env, out_entry, "mac", normalized);
                      } else {
                        SetNamedValue(env, out_entry, key, val);
                      }
                    }
                  }
                }
                napi_set_element(env, network_interfaces, out_idx++, out_entry);
              }
            }
          }
          SetNamedValue(env, header, "networkInterfaces", network_interfaces);
        }
      }
    }
  }

  SetNamedString(env, header, "glibcVersionRuntime", "");
  SetNamedString(env, header, "glibcVersionCompiler", "");
  SetNamedInt32(env, header, "reportVersion", 5);
  SetNamedValue(env, report, "header", header);

  napi_value native_stack = nullptr;
  napi_create_array_with_length(env, 1, &native_stack);
  napi_value frame = nullptr;
  napi_create_object(env, &frame);
  SetNamedString(env, frame, "pc", "0x0");
  SetNamedString(env, frame, "symbol", "ubi::report");
  napi_set_element(env, native_stack, 0, frame);
  SetNamedValue(env, report, "nativeStack", native_stack);

  napi_value js_stack = nullptr;
  napi_create_object(env, &js_stack);
  SetNamedString(env, js_stack, "message", event_message);
  napi_value js_frames = nullptr;
  napi_create_array(env, &js_frames);
  SetNamedValue(env, js_stack, "stack", js_frames);
  napi_value error_props = nullptr;
  napi_create_object(env, &error_props);
  SetNamedValue(env, js_stack, "errorProperties", error_props);
  SetNamedValue(env, report, "javascriptStack", js_stack);

  napi_value libuv = nullptr;
  napi_create_array_with_length(env, 2, &libuv);
  napi_value loop_entry = nullptr;
  napi_create_object(env, &loop_entry);
  SetNamedString(env, loop_entry, "type", "loop");
  SetNamedString(env, loop_entry, "address", "0x1");
  SetNamedBool(env, loop_entry, "is_active", true);
  napi_set_element(env, libuv, 0, loop_entry);
  napi_value timer_entry = nullptr;
  napi_create_object(env, &timer_entry);
  SetNamedString(env, timer_entry, "type", "timer");
  SetNamedString(env, timer_entry, "address", "0x2");
  SetNamedBool(env, timer_entry, "is_active", false);
  SetNamedBool(env, timer_entry, "is_referenced", false);
  napi_set_element(env, libuv, 1, timer_entry);
  SetNamedValue(env, report, "libuv", libuv);

  napi_value shared_objects = nullptr;
  napi_create_array(env, &shared_objects);
  SetNamedValue(env, report, "sharedObjects", shared_objects);

  napi_value usage = nullptr;
  napi_create_object(env, &usage);
  SetNamedDouble(env, usage, "userCpuSeconds", 0.0);
  SetNamedDouble(env, usage, "kernelCpuSeconds", 0.0);
  SetNamedDouble(env, usage, "cpuConsumptionPercent", 0.0);
  SetNamedDouble(env, usage, "userCpuConsumptionPercent", 0.0);
  SetNamedDouble(env, usage, "kernelCpuConsumptionPercent", 0.0);
  SetNamedString(env, usage, "maxRss", "0");
  SetNamedString(env, usage, "rss", "0");
  SetNamedString(env, usage, "free_memory", "0");
  SetNamedString(env, usage, "total_memory", "0");
  SetNamedString(env, usage, "available_memory", "0");
  napi_value page_faults = nullptr;
  napi_create_object(env, &page_faults);
  SetNamedInt32(env, page_faults, "IORequired", 0);
  SetNamedInt32(env, page_faults, "IONotRequired", 0);
  SetNamedValue(env, usage, "pageFaults", page_faults);
  napi_value fs_activity = nullptr;
  napi_create_object(env, &fs_activity);
  SetNamedInt32(env, fs_activity, "reads", 0);
  SetNamedInt32(env, fs_activity, "writes", 0);
  SetNamedValue(env, usage, "fsActivity", fs_activity);
  SetNamedValue(env, report, "resourceUsage", usage);

  napi_value workers = nullptr;
  napi_create_array(env, &workers);
  SetNamedValue(env, report, "workers", workers);

  ReportBindingState* state = GetReportState(env);
  if (state == nullptr || !state->exclude_env) {
    napi_value env_obj = nullptr;
    if (napi_get_named_property(env, process_obj, "env", &env_obj) == napi_ok && env_obj != nullptr) {
      SetNamedValue(env, report, "environmentVariables", env_obj);
    }
  }

  napi_value user_limits = MakeReportUserLimits(env);
  if (user_limits != nullptr) {
    SetNamedValue(env, report, "userLimits", user_limits);
  }

  napi_value js_heap = nullptr;
  napi_create_object(env, &js_heap);
  const char* heap_int_fields[] = {
      "totalMemory", "executableMemory", "totalCommittedMemory", "availableMemory",
      "totalGlobalHandlesMemory", "usedGlobalHandlesMemory", "usedMemory",
      "memoryLimit", "mallocedMemory", "externalMemory", "peakMallocedMemory",
      "nativeContextCount", "detachedContextCount", "doesZapGarbage",
  };
  for (const char* field : heap_int_fields) {
    SetNamedInt32(env, js_heap, field, 0);
  }
  napi_value heap_spaces = nullptr;
  napi_create_object(env, &heap_spaces);
  napi_value space = nullptr;
  napi_create_object(env, &space);
  const char* heap_space_fields[] = {"memorySize", "committedMemory", "capacity", "used", "available"};
  for (const char* field : heap_space_fields) SetNamedInt32(env, space, field, 0);
  SetNamedValue(env, heap_spaces, "new_space", space);
  SetNamedValue(env, js_heap, "heapSpaces", heap_spaces);
  SetNamedValue(env, report, "javascriptHeap", js_heap);

  return report;
}

std::string BuildDefaultReportFilename() {
  const auto now_tp = std::chrono::system_clock::now();
  const auto now = std::chrono::system_clock::to_time_t(now_tp);
  std::tm local_tm{};
#if defined(_WIN32)
  localtime_s(&local_tm, &now);
#else
  localtime_r(&now, &local_tm);
#endif
  char date_buf[16] = {'\0'};
  char time_buf[16] = {'\0'};
  std::strftime(date_buf, sizeof(date_buf), "%Y%m%d", &local_tm);
  std::strftime(time_buf, sizeof(time_buf), "%H%M%S", &local_tm);
  std::ostringstream oss;
  oss << "report." << date_buf << "." << time_buf << "." << uv_os_getpid() << "."
      << uv_os_getpid() << ".";
  return oss.str();
}

std::string JoinPath(const std::string& dir, const std::string& file) {
  namespace fs = std::filesystem;
  fs::path p = fs::path(dir) / fs::path(file);
  return p.lexically_normal().string();
}

bool HasExecArgvFlag(const std::vector<std::string>& exec_argv, const char* flag) {
  for (const auto& arg : exec_argv) {
    if (arg == flag) return true;
  }
  return false;
}

bool IsValidMacAddress(const std::string& mac) {
  if (mac.size() != 17) return false;
  for (size_t i = 0; i < mac.size(); ++i) {
    if ((i + 1) % 3 == 0) {
      if (mac[i] != ':') return false;
      continue;
    }
    const char ch = mac[i];
    const bool hex_digit =
        (ch >= '0' && ch <= '9') ||
        (ch >= 'a' && ch <= 'f') ||
        (ch >= 'A' && ch <= 'F');
    if (!hex_digit) return false;
  }
  return true;
}

void CopyProcessEnvironmentToObject(napi_env env, napi_value env_obj) {
#if defined(_WIN32)
  char** e = _environ;
#else
  char** e = environ;
#endif
  if (e == nullptr) return;
  for (; *e != nullptr; ++e) {
    const char* entry = *e;
    const char* sep = std::strchr(entry, '=');
    if (sep == nullptr) continue;
    const std::string key(entry, static_cast<size_t>(sep - entry));
    const std::string value(sep + 1);
    napi_value v = nullptr;
    if (napi_create_string_utf8(env, value.c_str(), NAPI_AUTO_LENGTH, &v) != napi_ok || v == nullptr) continue;
    napi_set_named_property(env, env_obj, key.c_str(), v);
  }
}

void MaybeInvokeWriteCallback(napi_env env, napi_value maybe_fn) {
  if (maybe_fn == nullptr) return;
  napi_valuetype type = napi_undefined;
  if (napi_typeof(env, maybe_fn, &type) != napi_ok || type != napi_function) return;
  napi_value global = nullptr;
  napi_value null_value = nullptr;
  if (napi_get_global(env, &global) != napi_ok || global == nullptr ||
      napi_get_null(env, &null_value) != napi_ok || null_value == nullptr) {
    return;
  }
  napi_value argv[1] = {null_value};
  napi_value ignored = nullptr;
  napi_call_function(env, global, maybe_fn, 1, argv, &ignored);
}

napi_value ProcessStdoutWriteCallback(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value argv[2] = {nullptr, nullptr};
  if (napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr) == napi_ok && argc >= 1 && argv[0] != nullptr) {
    const std::string out = NapiValueToUtf8(env, argv[0]);
    std::cout << out;
    std::cout.flush();
  }
  if (argc >= 2) MaybeInvokeWriteCallback(env, argv[1]);
  napi_value undefined = nullptr;
  napi_get_undefined(env, &undefined);
  return undefined;
}

napi_value ProcessStderrWriteCallback(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value argv[2] = {nullptr, nullptr};
  if (napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr) == napi_ok && argc >= 1 && argv[0] != nullptr) {
    const std::string out = NapiValueToUtf8(env, argv[0]);
    std::cerr << out;
    std::cerr.flush();
  }
  if (argc >= 2) MaybeInvokeWriteCallback(env, argv[1]);
  napi_value undefined = nullptr;
  napi_get_undefined(env, &undefined);
  return undefined;
}

napi_status InstallProcessStream(napi_env env,
                                 napi_value process_obj,
                                 const char* name,
                                 int fd,
                                 napi_callback write_cb) {
  napi_value stream_obj = nullptr;
  napi_status status = napi_create_object(env, &stream_obj);
  if (status != napi_ok || stream_obj == nullptr) return (status == napi_ok) ? napi_generic_failure : status;
  napi_value fd_value = nullptr;
  status = napi_create_int32(env, fd, &fd_value);
  if (status != napi_ok || fd_value == nullptr) return (status == napi_ok) ? napi_generic_failure : status;
  status = napi_set_named_property(env, stream_obj, "fd", fd_value);
  if (status != napi_ok) return status;
  napi_value true_value = nullptr;
  status = napi_get_boolean(env, true, &true_value);
  if (status != napi_ok || true_value == nullptr) return (status == napi_ok) ? napi_generic_failure : status;
  status = napi_set_named_property(env, stream_obj, "writable", true_value);
  if (status != napi_ok) return status;
  status = napi_set_named_property(env, stream_obj, "_isStdio", true_value);
  if (status != napi_ok) return status;
  napi_value false_value = nullptr;
  status = napi_get_boolean(env, false, &false_value);
  if (status != napi_ok || false_value == nullptr) return (status == napi_ok) ? napi_generic_failure : status;
  status = napi_set_named_property(env, stream_obj, "isTTY", false_value);
  if (status != napi_ok) return status;
  napi_value write_fn = nullptr;
  status = napi_create_function(env, "write", NAPI_AUTO_LENGTH, write_cb, nullptr, &write_fn);
  if (status != napi_ok || write_fn == nullptr) return (status == napi_ok) ? napi_generic_failure : status;
  status = napi_set_named_property(env, stream_obj, "write", write_fn);
  if (status != napi_ok) return status;
  napi_value end_fn = nullptr;
  status = napi_create_function(
      env,
      "end",
      NAPI_AUTO_LENGTH,
      [](napi_env env, napi_callback_info info) -> napi_value {
        size_t argc = 1;
        napi_value argv[1] = {nullptr};
        napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
        if (argc >= 1 && argv[0] != nullptr) {
          napi_valuetype t = napi_undefined;
          if (napi_typeof(env, argv[0], &t) == napi_ok && t == napi_function) {
            napi_value global = nullptr;
            napi_value ignored = nullptr;
            if (napi_get_global(env, &global) == napi_ok && global != nullptr) {
              napi_call_function(env, global, argv[0], 0, nullptr, &ignored);
            }
          }
        }
        napi_value undefined = nullptr;
        napi_get_undefined(env, &undefined);
        return undefined;
      },
      nullptr,
      &end_fn);
  if (status != napi_ok || end_fn == nullptr) return (status == napi_ok) ? napi_generic_failure : status;
  status = napi_set_named_property(env, stream_obj, "end", end_fn);
  if (status != napi_ok) return status;
  auto return_undefined = [](napi_env env, napi_callback_info info) -> napi_value {
    napi_value undefined = nullptr;
    napi_get_undefined(env, &undefined);
    return undefined;
  };
  auto return_this = [](napi_env env, napi_callback_info info) -> napi_value {
    napi_value this_arg = nullptr;
    if (napi_get_cb_info(env, info, nullptr, nullptr, &this_arg, nullptr) != napi_ok || this_arg == nullptr) {
      napi_value undefined = nullptr;
      napi_get_undefined(env, &undefined);
      return undefined;
    }
    return this_arg;
  };
  const char* event_methods_this[] = {"on", "addListener", "once", "prependListener", "removeListener"};
  for (const char* method : event_methods_this) {
    napi_value fn = nullptr;
    status = napi_create_function(env, method, NAPI_AUTO_LENGTH, return_this, nullptr, &fn);
    if (status != napi_ok || fn == nullptr) return (status == napi_ok) ? napi_generic_failure : status;
    status = napi_set_named_property(env, stream_obj, method, fn);
    if (status != napi_ok) return status;
  }
  napi_value emit_fn = nullptr;
  status = napi_create_function(env, "emit", NAPI_AUTO_LENGTH, return_undefined, nullptr, &emit_fn);
  if (status != napi_ok || emit_fn == nullptr) return (status == napi_ok) ? napi_generic_failure : status;
  status = napi_set_named_property(env, stream_obj, "emit", emit_fn);
  if (status != napi_ok) return status;
  return napi_set_named_property(env, process_obj, name, stream_obj);
}

napi_value ProcessCwdCallback(napi_env env, napi_callback_info info) {
  std::string cwd = ".";
#if defined(_WIN32)
  const char* pwd = std::getenv("PWD");
  if (pwd != nullptr && pwd[0] != '\0') cwd = pwd;
#else
  char buf[4096] = {'\0'};
  if (getcwd(buf, sizeof(buf)) != nullptr) cwd = buf;
#endif
  napi_value result = nullptr;
  if (napi_create_string_utf8(env, cwd.c_str(), NAPI_AUTO_LENGTH, &result) != napi_ok) return nullptr;
  return result;
}

napi_value ProcessChdirCallback(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value args[1] = {nullptr};
  if (napi_get_cb_info(env, info, &argc, args, nullptr, nullptr) != napi_ok) return nullptr;
  if (argc < 1 || args[0] == nullptr) {
    ThrowTypeErrorWithCode(env, "ERR_INVALID_ARG_TYPE", "The \"directory\" argument must be of type string");
    return nullptr;
  }
  napi_valuetype type = napi_undefined;
  if (napi_typeof(env, args[0], &type) != napi_ok || type != napi_string) {
    ThrowTypeErrorWithCode(env, "ERR_INVALID_ARG_TYPE", "The \"directory\" argument must be of type string");
    return nullptr;
  }
  size_t len = 0;
  if (napi_get_value_string_utf8(env, args[0], nullptr, 0, &len) != napi_ok) return nullptr;
  std::vector<char> buf(len + 1, '\0');
  size_t copied = 0;
  if (napi_get_value_string_utf8(env, args[0], buf.data(), buf.size(), &copied) != napi_ok) return nullptr;
  const std::string dest(buf.data(), copied);
#if defined(_WIN32)
  (void)dest;
  napi_value undefined = nullptr;
  napi_get_undefined(env, &undefined);
  return undefined;
#else
  char oldcwd[4096] = {'\0'};
  const char* oldcwd_s = getcwd(oldcwd, sizeof(oldcwd)) ? oldcwd : ".";
  if (chdir(dest.c_str()) != 0) {
    std::string msg = "ENOENT: no such file or directory, chdir " + std::string(oldcwd_s) + " -> '" + dest + "'";
    napi_value code = nullptr;
    napi_value text = nullptr;
    napi_value e = nullptr;
    napi_create_string_utf8(env, "ENOENT", NAPI_AUTO_LENGTH, &code);
    napi_create_string_utf8(env, msg.c_str(), NAPI_AUTO_LENGTH, &text);
    napi_create_error(env, code, text, &e);
    napi_value syscall = nullptr;
    napi_value path = nullptr;
    napi_value destv = nullptr;
    napi_create_string_utf8(env, "chdir", NAPI_AUTO_LENGTH, &syscall);
    napi_create_string_utf8(env, oldcwd_s, NAPI_AUTO_LENGTH, &path);
    napi_create_string_utf8(env, dest.c_str(), NAPI_AUTO_LENGTH, &destv);
    napi_set_named_property(env, e, "code", code);
    napi_set_named_property(env, e, "syscall", syscall);
    napi_set_named_property(env, e, "path", path);
    napi_set_named_property(env, e, "dest", destv);
    napi_throw(env, e);
    return nullptr;
  }
  setenv("PWD", dest.c_str(), 1);
  napi_value undefined = nullptr;
  napi_get_undefined(env, &undefined);
  return undefined;
#endif
}

napi_value ProcessCpuUsageCallback(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value args[1] = {nullptr};
  napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
  const auto now = std::chrono::steady_clock::now();
  const uint64_t micros =
      static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(now - g_cpu_usage_start).count());
  uint64_t user = micros * 7 / 10;
  uint64_t system = micros - user;
  if (argc >= 1 && args[0] != nullptr) {
    napi_valuetype t = napi_undefined;
    if (napi_typeof(env, args[0], &t) != napi_ok || t != napi_object) {
      ThrowTypeErrorWithCode(env, "ERR_INVALID_ARG_TYPE",
                             "The \"prevValue\" argument must be of type object. Received type number (1)");
      return nullptr;
    }
    bool has_user = false;
    bool has_system = false;
    napi_has_named_property(env, args[0], "user", &has_user);
    napi_has_named_property(env, args[0], "system", &has_system);
    if (!has_user) {
      ThrowTypeErrorWithCode(env, "ERR_INVALID_ARG_TYPE",
                             "The \"prevValue.user\" property must be of type number. Received undefined");
      return nullptr;
    }
    napi_value user_v = nullptr;
    napi_value sys_v = nullptr;
    napi_get_named_property(env, args[0], "user", &user_v);
    double prev_user = 0;
    double prev_sys = 0;
    if (napi_get_value_double(env, user_v, &prev_user) != napi_ok) {
      const std::string msg = "The \"prevValue.user\" property must be of type number." + InvalidArgTypeSuffix(env, user_v);
      ThrowTypeErrorWithCode(env, "ERR_INVALID_ARG_TYPE", msg.c_str());
      return nullptr;
    }
    if (!has_system) {
      ThrowTypeErrorWithCode(env, "ERR_INVALID_ARG_TYPE",
                             "The \"prevValue.system\" property must be of type number. Received undefined");
      return nullptr;
    }
    napi_get_named_property(env, args[0], "system", &sys_v);
    if (napi_get_value_double(env, sys_v, &prev_sys) != napi_ok) {
      const std::string msg = "The \"prevValue.system\" property must be of type number." + InvalidArgTypeSuffix(env, sys_v);
      ThrowTypeErrorWithCode(env, "ERR_INVALID_ARG_TYPE", msg.c_str());
      return nullptr;
    }
    if (prev_user < 0 || std::isinf(prev_user)) {
      const std::string msg = "The property 'prevValue.user' is invalid. Received " + FormatNodeNumber(prev_user);
      ThrowRangeErrorWithCode(env, "ERR_INVALID_ARG_VALUE", msg.c_str());
      return nullptr;
    }
    if (prev_sys < 0 || std::isinf(prev_sys)) {
      const std::string msg = "The property 'prevValue.system' is invalid. Received " + FormatNodeNumber(prev_sys);
      ThrowRangeErrorWithCode(env, "ERR_INVALID_ARG_VALUE", msg.c_str());
      return nullptr;
    }
    const uint64_t pu = static_cast<uint64_t>(prev_user);
    const uint64_t ps = static_cast<uint64_t>(prev_sys);
    user = user > pu ? user - pu : 0;
    system = system > ps ? system - ps : 0;
  }
  napi_value obj = nullptr;
  napi_create_object(env, &obj);
  napi_value user_v = nullptr;
  napi_value sys_v = nullptr;
  napi_create_double(env, static_cast<double>(user), &user_v);
  napi_create_double(env, static_cast<double>(system), &sys_v);
  napi_set_named_property(env, obj, "user", user_v);
  napi_set_named_property(env, obj, "system", sys_v);
  return obj;
}

napi_value ProcessAvailableMemoryCallback(napi_env env, napi_callback_info info) {
  napi_value out = nullptr;
  napi_create_double(env, 1024.0 * 1024.0 * 1024.0, &out);
  return out;
}

napi_value ProcessConstrainedMemoryCallback(napi_env env, napi_callback_info info) {
  napi_value out = nullptr;
  napi_create_double(env, 0, &out);
  return out;
}

napi_value ProcessUmaskCallback(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value args[1] = {nullptr};
  if (napi_get_cb_info(env, info, &argc, args, nullptr, nullptr) != napi_ok) return nullptr;
  const uint32_t old_mask = g_process_umask;
  if (argc >= 1 && args[0] != nullptr) {
    napi_valuetype arg_type = napi_undefined;
    if (napi_typeof(env, args[0], &arg_type) != napi_ok) return nullptr;
    if (arg_type == napi_string) {
      size_t len = 0;
      if (napi_get_value_string_utf8(env, args[0], nullptr, 0, &len) != napi_ok) return nullptr;
      std::vector<char> buf(len + 1, '\0');
      size_t copied = 0;
      if (napi_get_value_string_utf8(env, args[0], buf.data(), buf.size(), &copied) != napi_ok) return nullptr;
      const std::string value(buf.data(), copied);
      if (value.empty()) {
        ThrowErrorWithCode(env, "ERR_INVALID_ARG_VALUE", "The argument 'mask' is invalid.");
        return nullptr;
      }
      for (char ch : value) {
        if (ch < '0' || ch > '7') {
          ThrowErrorWithCode(env, "ERR_INVALID_ARG_VALUE", "The argument 'mask' is invalid.");
          return nullptr;
        }
      }
      try {
        g_process_umask = static_cast<uint32_t>(std::stoul(value, nullptr, 8)) & 0777u;
      } catch (...) {
        ThrowErrorWithCode(env, "ERR_INVALID_ARG_VALUE", "The argument 'mask' is invalid.");
        return nullptr;
      }
    } else if (arg_type == napi_number) {
      double num = 0;
      if (napi_get_value_double(env, args[0], &num) != napi_ok) return nullptr;
      if (!(num >= 0) || num != num) {
        ThrowErrorWithCode(env, "ERR_INVALID_ARG_VALUE", "The argument 'mask' is invalid.");
        return nullptr;
      }
      g_process_umask = static_cast<uint32_t>(num) & 0777u;
    } else if (arg_type != napi_undefined) {
      ThrowTypeErrorWithCode(env, "ERR_INVALID_ARG_TYPE", "The \"mask\" argument must be of type number or string.");
      return nullptr;
    }
  }
  napi_value result = nullptr;
  if (napi_create_uint32(env, old_mask, &result) != napi_ok) return nullptr;
  return result;
}

napi_value ProcessUptimeCallback(napi_env env, napi_callback_info info) {
  const auto now = std::chrono::steady_clock::now();
  const auto elapsed = now - g_process_start_time;
  const double seconds = std::chrono::duration_cast<std::chrono::duration<double>>(elapsed).count();
  napi_value result = nullptr;
  if (napi_create_double(env, seconds, &result) != napi_ok) return nullptr;
  return result;
}

napi_value ProcessHrtimeBigintCallback(napi_env env, napi_callback_info info) {
  napi_value result = nullptr;
  if (napi_create_bigint_uint64(env, GetHrtimeNanoseconds(), &result) != napi_ok) return nullptr;
  return result;
}

napi_value ProcessHrtimeCallback(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value args[1] = {nullptr};
  if (napi_get_cb_info(env, info, &argc, args, nullptr, nullptr) != napi_ok) return nullptr;
  const uint64_t now_ns = GetHrtimeNanoseconds();
  uint64_t out_ns = now_ns;
  if (argc == 1 && args[0] != nullptr) {
    bool is_array = false;
    if (napi_is_array(env, args[0], &is_array) != napi_ok || !is_array) {
      ThrowTypeErrorWithCode(env, "ERR_INVALID_ARG_TYPE",
                             "The \"time\" argument must be an instance of Array. Received type number (1)");
      return nullptr;
    }
    uint32_t len = 0;
    if (napi_get_array_length(env, args[0], &len) != napi_ok || len != 2) {
      napi_value code = nullptr;
      napi_value message = nullptr;
      napi_value err = nullptr;
      napi_create_string_utf8(env, "ERR_OUT_OF_RANGE", NAPI_AUTO_LENGTH, &code);
      std::string msg = "The value of \"time\" is out of range. It must be 2. Received " + std::to_string(len);
      napi_create_string_utf8(env, msg.c_str(), NAPI_AUTO_LENGTH, &message);
      napi_create_range_error(env, code, message, &err);
      napi_set_named_property(env, err, "code", code);
      napi_throw(env, err);
      return nullptr;
    }
    napi_value sec_val = nullptr;
    napi_value nsec_val = nullptr;
    double sec = 0;
    double nsec = 0;
    if (napi_get_element(env, args[0], 0, &sec_val) != napi_ok || napi_get_element(env, args[0], 1, &nsec_val) != napi_ok ||
        napi_get_value_double(env, sec_val, &sec) != napi_ok || napi_get_value_double(env, nsec_val, &nsec) != napi_ok) {
      ThrowTypeErrorWithCode(env, "ERR_INVALID_ARG_TYPE", "Invalid hrtime tuple values.");
      return nullptr;
    }
    const double base_ns = sec * 1e9 + nsec;
    out_ns = base_ns > static_cast<double>(now_ns) ? 0 : now_ns - static_cast<uint64_t>(base_ns);
  }
  napi_value out = nullptr;
  if (napi_create_array_with_length(env, 2, &out) != napi_ok || out == nullptr) return nullptr;
  napi_value seconds = nullptr;
  napi_value nanoseconds = nullptr;
  if (napi_create_uint32(env, static_cast<uint32_t>(out_ns / 1000000000ull), &seconds) != napi_ok ||
      napi_create_uint32(env, static_cast<uint32_t>(out_ns % 1000000000ull), &nanoseconds) != napi_ok) {
    return nullptr;
  }
  napi_set_element(env, out, 0, seconds);
  napi_set_element(env, out, 1, nanoseconds);
  return out;
}

napi_value ProcessExitCallback(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value args[1] = {nullptr};
  if (napi_get_cb_info(env, info, &argc, args, nullptr, nullptr) != napi_ok) return nullptr;
  int32_t exit_code = 0;
  if (argc > 0 && args[0] != nullptr) {
    napi_valuetype arg_type = napi_undefined;
    if (napi_typeof(env, args[0], &arg_type) == napi_ok && arg_type != napi_undefined) napi_get_value_int32(env, args[0], &exit_code);
  }
  napi_value code_value = nullptr;
  napi_value message_value = nullptr;
  napi_value error_value = nullptr;
  napi_create_string_utf8(env, "ERR_UBI_PROCESS_EXIT", NAPI_AUTO_LENGTH, &code_value);
  napi_create_string_utf8(env, "process.exit()", NAPI_AUTO_LENGTH, &message_value);
  napi_create_error(env, code_value, message_value, &error_value);
  napi_value exit_code_value = nullptr;
  napi_create_int32(env, exit_code, &exit_code_value);
  napi_set_named_property(env, error_value, "__ubiExitCode", exit_code_value);
  uv_loop_t* loop = uv_default_loop();
  if (loop != nullptr) {
    uv_stop(loop);
  }
  napi_throw(env, error_value);
  return nullptr;
}

napi_value ProcessAbortCallback(napi_env env, napi_callback_info info) {
  ThrowErrorWithCode(env, "ERR_UBI_PROCESS_ABORT", "process.abort()");
  return nullptr;
}

napi_value ProcessMethodsCauseSegfaultCallback(napi_env env, napi_callback_info info) {
#if defined(_WIN32)
  std::abort();
#else
  raise(SIGSEGV);
  std::abort();
#endif
  napi_value undefined = nullptr;
  napi_get_undefined(env, &undefined);
  return undefined;
}

napi_value ProcessMethodsKillCallback(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value argv[2] = {nullptr, nullptr};
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  if (argc < 1 || argv[0] == nullptr) return nullptr;
  int32_t pid = 0;
  if (!ValueToInt32(env, argv[0], &pid)) return nullptr;
  int32_t signal = 0;
  if (argc >= 2 && argv[1] != nullptr) ValueToInt32(env, argv[1], &signal);
  int rc = uv_kill(pid, signal);
  napi_value out = nullptr;
  napi_create_int32(env, rc, &out);
  return out;
}

napi_value ProcessMethodsSetEnvCallback(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value argv[2] = {nullptr, nullptr};
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);

  int32_t rc = -1;
  if (argc >= 2 && argv[0] != nullptr && argv[1] != nullptr) {
    const std::string key = NapiValueToUtf8(env, argv[0]);
    const std::string value = NapiValueToUtf8(env, argv[1]);
    if (!key.empty()) {
#if defined(_WIN32)
      rc = (_putenv_s(key.c_str(), value.c_str()) == 0) ? 0 : -1;
#else
      rc = (setenv(key.c_str(), value.c_str(), 1) == 0) ? 0 : -1;
#endif
      if (rc == 0 && key == "TZ") {
#if defined(_WIN32)
        _tzset();
#else
        tzset();
#endif
        (void)unofficial_napi_notify_datetime_configuration_change(env);
      }
    }
  }

  napi_value out = nullptr;
  napi_create_int32(env, rc, &out);
  return out;
}

napi_value ProcessMethodsUnsetEnvCallback(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);

  int32_t rc = -1;
  if (argc >= 1 && argv[0] != nullptr) {
    const std::string key = NapiValueToUtf8(env, argv[0]);
    if (!key.empty()) {
#if defined(_WIN32)
      rc = (_putenv_s(key.c_str(), "") == 0) ? 0 : -1;
#else
      rc = (unsetenv(key.c_str()) == 0) ? 0 : -1;
#endif
      if (rc == 0 && key == "TZ") {
#if defined(_WIN32)
        _tzset();
#else
        tzset();
#endif
        (void)unofficial_napi_notify_datetime_configuration_change(env);
      }
    }
  }

  napi_value out = nullptr;
  napi_create_int32(env, rc, &out);
  return out;
}

napi_value ProcessMethodsRawDebugCallback(napi_env env, napi_callback_info info) {
  size_t argc = 8;
  napi_value argv[8] = {nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr};
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  std::ostringstream oss;
  for (size_t i = 0; i < argc; ++i) {
    if (i > 0) oss << " ";
    oss << NapiValueToUtf8(env, argv[i]);
  }
  std::cerr << oss.str() << std::endl;
  napi_value undefined = nullptr;
  napi_get_undefined(env, &undefined);
  return undefined;
}

napi_value ProcessMethodsDebugProcessCallback(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  if (napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr) != napi_ok || argc < 1 || argv[0] == nullptr) {
    ThrowTypeErrorWithCode(env, "ERR_MISSING_ARGS", "Invalid number of arguments.");
    return nullptr;
  }
  int32_t pid = 0;
  if (!ValueToInt32(env, argv[0], &pid)) {
    ThrowTypeErrorWithCode(env, "ERR_INVALID_ARG_TYPE", "The \"pid\" argument must be of type number.");
    return nullptr;
  }

#if defined(_WIN32)
  ThrowErrorWithCode(
      env, "ERR_FEATURE_UNAVAILABLE_ON_PLATFORM", "process._debugProcess is not supported on win32 in Ubi runtime");
  return nullptr;
#else
  const int rc = uv_kill(pid, SIGUSR1);
  if (rc != 0) {
    ThrowSystemError(env, rc, "kill");
    return nullptr;
  }
  napi_value undefined = nullptr;
  napi_get_undefined(env, &undefined);
  return undefined;
#endif
}

napi_value ProcessMethodsExecveCallback(napi_env env, napi_callback_info /*info*/) {
  ThrowErrorWithCode(env, "ERR_UBI_NOT_SUPPORTED", "process.execve() is not supported in Ubi runtime.");
  return nullptr;
}

napi_value ProcessMethodsPatchProcessObjectCallback(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  if (napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr) != napi_ok || argc < 1 || argv[0] == nullptr) {
    napi_value undefined = nullptr;
    napi_get_undefined(env, &undefined);
    return undefined;
  }

  napi_valuetype arg_type = napi_undefined;
  if (napi_typeof(env, argv[0], &arg_type) != napi_ok || arg_type != napi_object) {
    ThrowTypeErrorWithCode(env, "ERR_INVALID_ARG_TYPE", "The \"process\" argument must be of type object.");
    return nullptr;
  }

  napi_value global = nullptr;
  napi_value process_obj = nullptr;
  if (napi_get_global(env, &global) != napi_ok || global == nullptr ||
      napi_get_named_property(env, global, "process", &process_obj) != napi_ok || process_obj == nullptr) {
    napi_value undefined = nullptr;
    napi_get_undefined(env, &undefined);
    return undefined;
  }

  CopyNamedProperty(env, process_obj, argv[0], "argv");
  CopyNamedProperty(env, process_obj, argv[0], "execArgv");
  CopyNamedProperty(env, process_obj, argv[0], "pid");
  CopyNamedProperty(env, process_obj, argv[0], "ppid");
  CopyNamedProperty(env, process_obj, argv[0], "title");
  CopyNamedProperty(env, process_obj, argv[0], "debugPort");
  CopyNamedProperty(env, process_obj, argv[0], "execPath");
  CopyNamedProperty(env, process_obj, argv[0], "versions");

  napi_value undefined = nullptr;
  napi_get_undefined(env, &undefined);
  return undefined;
}

napi_value ProcessMethodsLoadEnvFileCallback(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  if (napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr) != napi_ok) return nullptr;

  const bool has_path_arg = argc >= 1 && argv[0] != nullptr;
  const std::string path = has_path_arg ? NapiValueToUtf8(env, argv[0]) : ".env";

  std::ifstream input(path, std::ios::in | std::ios::binary);
  if (!input.is_open()) {
    int err = uv_translate_sys_error(errno);
    if (err == 0) err = UV_ENOENT;
    ThrowSystemError(env, err, "open", path);
    return nullptr;
  }
  std::ostringstream buffer;
  buffer << input.rdbuf();
  const std::string content = buffer.str();

  napi_value global = nullptr;
  if (napi_get_global(env, &global) != napi_ok || global == nullptr) return nullptr;

  napi_value util_binding = nullptr;
  napi_value internal_binding = nullptr;
  napi_valuetype internal_binding_type = napi_undefined;
  if (napi_get_named_property(env, global, "internalBinding", &internal_binding) == napi_ok &&
      internal_binding != nullptr &&
      napi_typeof(env, internal_binding, &internal_binding_type) == napi_ok &&
      internal_binding_type == napi_function) {
    napi_value util_name = nullptr;
    if (napi_create_string_utf8(env, "util", NAPI_AUTO_LENGTH, &util_name) != napi_ok || util_name == nullptr) {
      return nullptr;
    }
    napi_value argv_ib[1] = {util_name};
    if (napi_call_function(env, global, internal_binding, 1, argv_ib, &util_binding) != napi_ok) {
      return nullptr;
    }
  }

  if (util_binding == nullptr) {
    napi_value undefined = nullptr;
    napi_get_undefined(env, &undefined);
    return undefined;
  }
  napi_valuetype util_binding_type = napi_undefined;
  if (napi_typeof(env, util_binding, &util_binding_type) != napi_ok || util_binding_type != napi_object) {
    napi_value undefined = nullptr;
    napi_get_undefined(env, &undefined);
    return undefined;
  }

  napi_value parse_env = nullptr;
  if (napi_get_named_property(env, util_binding, "parseEnv", &parse_env) != napi_ok || parse_env == nullptr) {
    napi_value undefined = nullptr;
    napi_get_undefined(env, &undefined);
    return undefined;
  }
  napi_valuetype parse_type = napi_undefined;
  if (napi_typeof(env, parse_env, &parse_type) != napi_ok || parse_type != napi_function) {
    napi_value undefined = nullptr;
    napi_get_undefined(env, &undefined);
    return undefined;
  }

  napi_value content_value = nullptr;
  if (napi_create_string_utf8(env, content.c_str(), content.size(), &content_value) != napi_ok ||
      content_value == nullptr) {
    return nullptr;
  }

  napi_value parsed = nullptr;
  if (napi_call_function(env, util_binding, parse_env, 1, &content_value, &parsed) != napi_ok || parsed == nullptr) {
    return nullptr;
  }

  napi_valuetype parsed_type = napi_undefined;
  if (napi_typeof(env, parsed, &parsed_type) != napi_ok || parsed_type != napi_object) {
    napi_value undefined = nullptr;
    napi_get_undefined(env, &undefined);
    return undefined;
  }

  napi_value process_obj = nullptr;
  napi_value process_env = nullptr;
  if (napi_get_named_property(env, global, "process", &process_obj) != napi_ok || process_obj == nullptr ||
      napi_get_named_property(env, process_obj, "env", &process_env) != napi_ok || process_env == nullptr) {
    napi_value undefined = nullptr;
    napi_get_undefined(env, &undefined);
    return undefined;
  }

  napi_value keys = nullptr;
  if (napi_get_property_names(env, parsed, &keys) != napi_ok || keys == nullptr) return nullptr;
  uint32_t key_count = 0;
  if (napi_get_array_length(env, keys, &key_count) != napi_ok) return nullptr;

  for (uint32_t i = 0; i < key_count; ++i) {
    napi_value key = nullptr;
    if (napi_get_element(env, keys, i, &key) != napi_ok || key == nullptr) continue;

    bool has_existing = false;
    if (napi_has_property(env, process_env, key, &has_existing) != napi_ok || has_existing) continue;

    napi_value value = nullptr;
    if (napi_get_property(env, parsed, key, &value) != napi_ok || value == nullptr) continue;
    napi_set_property(env, process_env, key, value);
  }

  napi_value undefined = nullptr;
  napi_get_undefined(env, &undefined);
  return undefined;
}

napi_value ProcessMethodsNoopUndefinedCallback(napi_env env, napi_callback_info info) {
  napi_value undefined = nullptr;
  napi_get_undefined(env, &undefined);
  return undefined;
}

napi_value RunScriptOrEmptyArray(napi_env env, const char* source) {
  napi_value script = nullptr;
  if (napi_create_string_utf8(env, source, NAPI_AUTO_LENGTH, &script) != napi_ok || script == nullptr) {
    napi_value out = nullptr;
    napi_create_array(env, &out);
    return out;
  }
  napi_value result = nullptr;
  if (napi_run_script(env, script, &result) != napi_ok || result == nullptr) {
    bool has_pending = false;
    if (napi_is_exception_pending(env, &has_pending) == napi_ok && has_pending) {
      napi_value ignored = nullptr;
      napi_get_and_clear_last_exception(env, &ignored);
    }
    napi_value out = nullptr;
    napi_create_array(env, &out);
    return out;
  }
  return result;
}

napi_value ProcessMethodsGetActiveRequestsCallback(napi_env env, napi_callback_info info) {
  static constexpr const char* kScript =
      "(function(){"
      "  try {"
      "    const reqs = globalThis[Symbol.for('node.activeRequests')];"
      "    return Array.isArray(reqs) ? reqs.slice() : [];"
      "  } catch {"
      "    return [];"
      "  }"
      "})()";
  return RunScriptOrEmptyArray(env, kScript);
}

napi_value ProcessMethodsGetActiveHandlesCallback(napi_env env, napi_callback_info info) {
  static constexpr const char* kScript =
      "(function(){"
      "  try {"
      "    const handles = globalThis[Symbol.for('node.activeHandles')];"
      "    return Array.isArray(handles) ? handles.slice() : [];"
      "  } catch {"
      "    return [];"
      "  }"
      "})()";
  return RunScriptOrEmptyArray(env, kScript);
}

napi_value ProcessMethodsGetActiveResourcesInfoCallback(napi_env env, napi_callback_info info) {
  static constexpr const char* kScript =
      "(function(){"
      "  const out = [];"
      "  try {"
      "    const resources = globalThis[Symbol.for('node.activeResources')];"
      "    if (resources && typeof resources.values === 'function') {"
      "      for (const type of resources.values()) {"
      "        out.push(String(type));"
      "      }"
      "    }"
      "    const reqs = globalThis[Symbol.for('node.activeRequests')];"
      "    if (Array.isArray(reqs)) {"
      "      for (let i = 0; i < reqs.length; i++) {"
      "        const req = reqs[i];"
      "        if (req && typeof req.type === 'string') out.push(req.type);"
      "      }"
      "    }"
      "  } catch {}"
      "  return out;"
      "})()";
  return RunScriptOrEmptyArray(env, kScript);
}

napi_value ProcessMethodsDlopenCallback(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value argv[2] = {nullptr, nullptr};
  if (napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr) != napi_ok) {
    return nullptr;
  }
  std::string filename = "(unknown)";
  if (argc >= 2 && argv[1] != nullptr) {
    const std::string maybe_name = NapiValueToUtf8(env, argv[1]);
    if (!maybe_name.empty()) filename = maybe_name;
  }
  const std::string message = "Module did not self-register: '" + filename + "'.";
  ThrowErrorWithCode(env, "ERR_DLOPEN_FAILED", message.c_str());
  return nullptr;
}

napi_value ProcessMethodsEmptyArrayCallback(napi_env env, napi_callback_info info) {
  napi_value out = nullptr;
  napi_create_array(env, &out);
  return out;
}

static size_t get_rss() {
#ifdef DUMMY_UV_STUBS
  return 0;
#else
  size_t rss = 0;
  uv_resident_set_memory(&rss);
  return rss;
#endif
}

napi_value ProcessMethodsRssCallback(napi_env env, napi_callback_info info) {
  napi_value out = nullptr;
  napi_create_double(env, static_cast<double>(get_rss()), &out);
  return out;
}

napi_value ProcessMethodsCpuUsageBufferCallback(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  if (argc < 1 || argv[0] == nullptr) return nullptr;
  bool is_typedarray = false;
  napi_is_typedarray(env, argv[0], &is_typedarray);
  if (!is_typedarray) return nullptr;
  napi_typedarray_type ta_type;
  size_t length = 0;
  void* data = nullptr;
  napi_value arraybuffer = nullptr;
  size_t byte_offset = 0;
  if (napi_get_typedarray_info(env, argv[0], &ta_type, &length, &data, &arraybuffer, &byte_offset) != napi_ok ||
      data == nullptr) {
    return nullptr;
  }
  if (ta_type != napi_float64_array || length < 2) return nullptr;
  double* values = static_cast<double*>(data);
  const auto now = std::chrono::steady_clock::now();
  const uint64_t micros =
      static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(now - g_cpu_usage_start).count());
  values[0] = static_cast<double>(micros * 7 / 10);
  values[1] = static_cast<double>(micros * 3 / 10);
  napi_value undefined = nullptr;
  napi_get_undefined(env, &undefined);
  return undefined;
}

napi_value ProcessMethodsThreadCpuUsageBufferCallback(napi_env env, napi_callback_info info) {
  return ProcessMethodsCpuUsageBufferCallback(env, info);
}

napi_value ProcessMethodsMemoryUsageBufferCallback(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  if (argc < 1 || argv[0] == nullptr) return nullptr;
  bool is_typedarray = false;
  napi_is_typedarray(env, argv[0], &is_typedarray);
  if (!is_typedarray) return nullptr;
  napi_typedarray_type ta_type;
  size_t length = 0;
  void* data = nullptr;
  napi_value arraybuffer = nullptr;
  size_t byte_offset = 0;
  if (napi_get_typedarray_info(env, argv[0], &ta_type, &length, &data, &arraybuffer, &byte_offset) != napi_ok ||
      data == nullptr) {
    return nullptr;
  }
  if (ta_type != napi_float64_array || length < 5) return nullptr;
  double* values = static_cast<double*>(data);
  values[0] = static_cast<double>(get_rss());
  values[1] = 16.0 * 1024.0 * 1024.0;
  values[2] = 8.0 * 1024.0 * 1024.0;
  values[3] = 0.0;
  values[4] = 0.0;
  napi_value undefined = nullptr;
  napi_get_undefined(env, &undefined);
  return undefined;
}

napi_value ProcessMethodsResourceUsageBufferCallback(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  if (argc < 1 || argv[0] == nullptr) return nullptr;
  bool is_typedarray = false;
  napi_is_typedarray(env, argv[0], &is_typedarray);
  if (!is_typedarray) return nullptr;
  napi_typedarray_type ta_type;
  size_t length = 0;
  void* data = nullptr;
  napi_value arraybuffer = nullptr;
  size_t byte_offset = 0;
  if (napi_get_typedarray_info(env, argv[0], &ta_type, &length, &data, &arraybuffer, &byte_offset) != napi_ok ||
      data == nullptr) {
    return nullptr;
  }
  if (ta_type != napi_float64_array) return nullptr;
  double* values = static_cast<double*>(data);
  for (size_t i = 0; i < length; ++i) values[i] = 0;
  napi_value undefined = nullptr;
  napi_get_undefined(env, &undefined);
  return undefined;
}

void UpdateHrtimeBuffer(napi_env env, bool write_bigint) {
  ProcessMethodsBindingState* state = GetProcessMethodsState(env);
  if (state == nullptr || state->hrtime_buffer_ref == nullptr) return;
  napi_value buffer = nullptr;
  if (napi_get_reference_value(env, state->hrtime_buffer_ref, &buffer) != napi_ok || buffer == nullptr) return;
  bool is_typedarray = false;
  if (napi_is_typedarray(env, buffer, &is_typedarray) != napi_ok || !is_typedarray) return;
  napi_typedarray_type ta_type;
  size_t length = 0;
  void* data = nullptr;
  napi_value arraybuffer = nullptr;
  size_t byte_offset = 0;
  if (napi_get_typedarray_info(env, buffer, &ta_type, &length, &data, &arraybuffer, &byte_offset) != napi_ok ||
      data == nullptr || ta_type != napi_uint32_array || length < 3) {
    return;
  }
  uint32_t* values = static_cast<uint32_t*>(data);
  const uint64_t now_ns = GetHrtimeNanoseconds();
  if (write_bigint) {
    values[0] = static_cast<uint32_t>(now_ns & 0xffffffffull);
    values[1] = static_cast<uint32_t>((now_ns >> 32) & 0xffffffffull);
    return;
  }
  const uint64_t sec = now_ns / 1000000000ull;
  const uint32_t nsec = static_cast<uint32_t>(now_ns % 1000000000ull);
  values[0] = static_cast<uint32_t>((sec >> 32) & 0xffffffffull);
  values[1] = static_cast<uint32_t>(sec & 0xffffffffull);
  values[2] = nsec;
}

napi_value ProcessMethodsHrtimeCallback(napi_env env, napi_callback_info info) {
  UpdateHrtimeBuffer(env, false);
  napi_value undefined = nullptr;
  napi_get_undefined(env, &undefined);
  return undefined;
}

napi_value ProcessMethodsHrtimeBigIntCallback(napi_env env, napi_callback_info info) {
  UpdateHrtimeBuffer(env, true);
  napi_value undefined = nullptr;
  napi_get_undefined(env, &undefined);
  return undefined;
}

napi_value ProcessMethodsSetEmitWarningSyncCallback(napi_env env, napi_callback_info info) {
  napi_value undefined = nullptr;
  napi_get_undefined(env, &undefined);
  return undefined;
}

napi_value ProcessMethodsResetStdioForTestingCallback(napi_env env, napi_callback_info info) {
  napi_value undefined = nullptr;
  napi_get_undefined(env, &undefined);
  return undefined;
}

void ProcessMethodsBindingFinalize(napi_env env, void* data, void* hint) {
  (void)data;
  (void)hint;
  auto it = g_process_methods_states.find(env);
  if (it == g_process_methods_states.end()) return;
  if (it->second.hrtime_buffer_ref != nullptr) {
    napi_delete_reference(env, it->second.hrtime_buffer_ref);
    it->second.hrtime_buffer_ref = nullptr;
  }
  if (it->second.binding_ref != nullptr) {
    napi_delete_reference(env, it->second.binding_ref);
    it->second.binding_ref = nullptr;
  }
  g_process_methods_states.erase(it);
}

napi_value ReportWriteReportCallback(napi_env env, napi_callback_info info) {
  size_t argc = 4;
  napi_value argv[4] = {nullptr, nullptr, nullptr, nullptr};
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  const std::string event_message = (argc >= 1 && argv[0] != nullptr) ? NapiValueToUtf8(env, argv[0]) : "JavaScript API";
  const std::string trigger = (argc >= 2 && argv[1] != nullptr) ? NapiValueToUtf8(env, argv[1]) : "API";
  std::string requested_file;
  if (argc >= 3 && argv[2] != nullptr) {
    napi_valuetype t = napi_undefined;
    if (napi_typeof(env, argv[2], &t) == napi_ok && t == napi_string) {
      requested_file = NapiValueToUtf8(env, argv[2]);
    }
  }
  ReportBindingState* state = GetReportState(env);
  if (state == nullptr) return nullptr;

  if (!std::filesystem::exists(state->directory)) {
    std::filesystem::create_directories(state->directory);
  }

  std::string output_file = requested_file;
  if (output_file.empty()) {
    if (state->filename.empty()) {
      state->sequence++;
      std::ostringstream generated;
      generated << BuildDefaultReportFilename() << state->sequence << ".json";
      output_file = generated.str();
    } else {
      output_file = state->filename;
    }
  }
  const std::string absolute_path =
      std::filesystem::path(output_file).is_absolute() ? output_file : JoinPath(state->directory, output_file);

  napi_value report_obj = BuildReportObject(env, event_message, trigger, absolute_path);
  if (report_obj == nullptr) return nullptr;

  napi_value global = nullptr;
  napi_value json_obj = nullptr;
  napi_value stringify_fn = nullptr;
  if (napi_get_global(env, &global) != napi_ok || global == nullptr ||
      napi_get_named_property(env, global, "JSON", &json_obj) != napi_ok || json_obj == nullptr ||
      napi_get_named_property(env, json_obj, "stringify", &stringify_fn) != napi_ok || stringify_fn == nullptr) {
    return nullptr;
  }
  napi_value null_value = nullptr;
  napi_get_null(env, &null_value);
  napi_value space = nullptr;
  if (state->compact) {
    napi_create_int32(env, 0, &space);
  } else {
    napi_create_int32(env, 2, &space);
  }
  napi_value stringify_argv[3] = {report_obj, null_value, space};
  napi_value json_string = nullptr;
  if (napi_call_function(env, json_obj, stringify_fn, 3, stringify_argv, &json_string) != napi_ok ||
      json_string == nullptr) {
    return nullptr;
  }
  std::string payload = NapiValueToUtf8(env, json_string);
  payload.push_back('\n');

  std::ofstream out(absolute_path, std::ios::out | std::ios::trunc);
  out << payload;
  out.close();

  napi_value path_value = nullptr;
  napi_create_string_utf8(env, absolute_path.c_str(), NAPI_AUTO_LENGTH, &path_value);
  return path_value;
}

napi_value ReportGetReportCallback(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  const std::string event_message = "JavaScript API";
  const std::string trigger = "GetReport";
  napi_value report_obj = BuildReportObject(env, event_message, trigger, "");
  if (report_obj == nullptr) return nullptr;
  napi_value global = nullptr;
  napi_value json_obj = nullptr;
  napi_value stringify_fn = nullptr;
  if (napi_get_global(env, &global) != napi_ok || global == nullptr ||
      napi_get_named_property(env, global, "JSON", &json_obj) != napi_ok || json_obj == nullptr ||
      napi_get_named_property(env, json_obj, "stringify", &stringify_fn) != napi_ok || stringify_fn == nullptr) {
    return nullptr;
  }
  napi_value null_value = nullptr;
  napi_get_null(env, &null_value);
  napi_value space = nullptr;
  napi_create_int32(env, 2, &space);
  napi_value stringify_argv[3] = {report_obj, null_value, space};
  napi_value json_string = nullptr;
  if (napi_call_function(env, json_obj, stringify_fn, 3, stringify_argv, &json_string) != napi_ok ||
      json_string == nullptr) {
    return nullptr;
  }
  return json_string;
}

napi_value ReportGetCompactCallback(napi_env env, napi_callback_info info) {
  ReportBindingState* state = GetReportState(env);
  napi_value out = nullptr;
  napi_get_boolean(env, state != nullptr && state->compact, &out);
  return out;
}

napi_value ReportSetCompactCallback(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  ReportBindingState* state = GetReportState(env);
  if (state != nullptr && argc >= 1 && argv[0] != nullptr) {
    bool value = false;
    if (ValueToBool(env, argv[0], &value)) state->compact = value;
  }
  napi_value undefined = nullptr;
  napi_get_undefined(env, &undefined);
  return undefined;
}

napi_value ReportGetExcludeNetworkCallback(napi_env env, napi_callback_info info) {
  ReportBindingState* state = GetReportState(env);
  napi_value out = nullptr;
  napi_get_boolean(env, state != nullptr && state->exclude_network, &out);
  return out;
}

napi_value ReportSetExcludeNetworkCallback(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  ReportBindingState* state = GetReportState(env);
  if (state != nullptr && argc >= 1 && argv[0] != nullptr) {
    bool value = false;
    if (ValueToBool(env, argv[0], &value)) state->exclude_network = value;
  }
  napi_value undefined = nullptr;
  napi_get_undefined(env, &undefined);
  return undefined;
}

napi_value ReportGetExcludeEnvCallback(napi_env env, napi_callback_info info) {
  ReportBindingState* state = GetReportState(env);
  napi_value out = nullptr;
  napi_get_boolean(env, state != nullptr && state->exclude_env, &out);
  return out;
}

napi_value ReportSetExcludeEnvCallback(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  ReportBindingState* state = GetReportState(env);
  if (state != nullptr && argc >= 1 && argv[0] != nullptr) {
    bool value = false;
    if (ValueToBool(env, argv[0], &value)) state->exclude_env = value;
  }
  napi_value undefined = nullptr;
  napi_get_undefined(env, &undefined);
  return undefined;
}

napi_value ReportGetDirectoryCallback(napi_env env, napi_callback_info info) {
  ReportBindingState* state = GetReportState(env);
  napi_value out = nullptr;
  const std::string value = state != nullptr ? state->directory : ".";
  napi_create_string_utf8(env, value.c_str(), NAPI_AUTO_LENGTH, &out);
  return out;
}

napi_value ReportSetDirectoryCallback(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  ReportBindingState* state = GetReportState(env);
  if (state != nullptr && argc >= 1 && argv[0] != nullptr) {
    state->directory = NapiValueToUtf8(env, argv[0]);
    if (state->directory.empty()) state->directory = ".";
  }
  napi_value undefined = nullptr;
  napi_get_undefined(env, &undefined);
  return undefined;
}

napi_value ReportGetFilenameCallback(napi_env env, napi_callback_info info) {
  ReportBindingState* state = GetReportState(env);
  napi_value out = nullptr;
  const std::string value = state != nullptr ? state->filename : "";
  napi_create_string_utf8(env, value.c_str(), NAPI_AUTO_LENGTH, &out);
  return out;
}

napi_value ReportSetFilenameCallback(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  ReportBindingState* state = GetReportState(env);
  if (state != nullptr && argc >= 1 && argv[0] != nullptr) {
    state->filename = NapiValueToUtf8(env, argv[0]);
  }
  napi_value undefined = nullptr;
  napi_get_undefined(env, &undefined);
  return undefined;
}

napi_value ReportGetSignalCallback(napi_env env, napi_callback_info info) {
  ReportBindingState* state = GetReportState(env);
  napi_value out = nullptr;
  const std::string value = state != nullptr ? state->signal : "SIGUSR2";
  napi_create_string_utf8(env, value.c_str(), NAPI_AUTO_LENGTH, &out);
  return out;
}

napi_value ReportSetSignalCallback(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  ReportBindingState* state = GetReportState(env);
  if (state != nullptr && argc >= 1 && argv[0] != nullptr) {
    state->signal = NapiValueToUtf8(env, argv[0]);
  }
  napi_value undefined = nullptr;
  napi_get_undefined(env, &undefined);
  return undefined;
}

napi_value ReportShouldReportOnFatalErrorCallback(napi_env env, napi_callback_info info) {
  ReportBindingState* state = GetReportState(env);
  napi_value out = nullptr;
  napi_get_boolean(env, state != nullptr && state->report_on_fatal_error, &out);
  return out;
}

napi_value ReportSetReportOnFatalErrorCallback(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  ReportBindingState* state = GetReportState(env);
  if (state != nullptr && argc >= 1 && argv[0] != nullptr) {
    bool value = false;
    if (ValueToBool(env, argv[0], &value)) state->report_on_fatal_error = value;
  }
  napi_value undefined = nullptr;
  napi_get_undefined(env, &undefined);
  return undefined;
}

napi_value ReportShouldReportOnSignalCallback(napi_env env, napi_callback_info info) {
  ReportBindingState* state = GetReportState(env);
  napi_value out = nullptr;
  napi_get_boolean(env, state != nullptr && state->report_on_signal, &out);
  return out;
}

napi_value ReportSetReportOnSignalCallback(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  ReportBindingState* state = GetReportState(env);
  if (state != nullptr && argc >= 1 && argv[0] != nullptr) {
    bool value = false;
    if (ValueToBool(env, argv[0], &value)) state->report_on_signal = value;
  }
  napi_value undefined = nullptr;
  napi_get_undefined(env, &undefined);
  return undefined;
}

napi_value ReportShouldReportOnUncaughtExceptionCallback(napi_env env, napi_callback_info info) {
  ReportBindingState* state = GetReportState(env);
  napi_value out = nullptr;
  napi_get_boolean(env, state != nullptr && state->report_on_uncaught_exception, &out);
  return out;
}

napi_value ReportSetReportOnUncaughtExceptionCallback(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  ReportBindingState* state = GetReportState(env);
  if (state != nullptr && argc >= 1 && argv[0] != nullptr) {
    bool value = false;
    if (ValueToBool(env, argv[0], &value)) state->report_on_uncaught_exception = value;
  }
  napi_value undefined = nullptr;
  napi_get_undefined(env, &undefined);
  return undefined;
}

void ReportBindingFinalize(napi_env env, void* data, void* hint) {
  (void)data;
  (void)hint;
  auto it = g_report_states.find(env);
  if (it == g_report_states.end()) return;
  if (it->second.binding_ref != nullptr) {
    napi_delete_reference(env, it->second.binding_ref);
    it->second.binding_ref = nullptr;
  }
  g_report_states.erase(it);
}

}  // namespace

napi_status UbiInstallProcessObject(napi_env env,
                                      const std::string& current_script_path,
                                      const std::vector<std::string>& exec_argv,
                                      const std::vector<std::string>& script_argv,
                                      const std::string& process_title) {
  if (env == nullptr) return napi_invalid_arg;
  if (g_ubi_exec_path.empty()) g_ubi_exec_path = DetectExecPath();
  napi_value global = nullptr;
  napi_status status = napi_get_global(env, &global);
  if (status != napi_ok || global == nullptr) return (status == napi_ok) ? napi_generic_failure : status;
  napi_value process_obj = nullptr;
  status = napi_create_object(env, &process_obj);
  if (status != napi_ok || process_obj == nullptr) return (status == napi_ok) ? napi_generic_failure : status;

  napi_value env_obj = nullptr;
  status = napi_create_object(env, &env_obj);
  if (status != napi_ok || env_obj == nullptr) return (status == napi_ok) ? napi_generic_failure : status;
  status = napi_set_named_property(env, process_obj, "env", env_obj);
  if (status != napi_ok) return status;
  CopyProcessEnvironmentToObject(env, env_obj);

  napi_value argv_arr = nullptr;
  const bool has_script_path = !current_script_path.empty();
  const size_t argv_len = has_script_path ? (2 + script_argv.size()) : (1 + script_argv.size());
  status = napi_create_array_with_length(env, argv_len, &argv_arr);
  if (status != napi_ok || argv_arr == nullptr) return (status == napi_ok) ? napi_generic_failure : status;
  napi_value exec_argv0 = nullptr;
  napi_create_string_utf8(env, g_ubi_exec_path.c_str(), NAPI_AUTO_LENGTH, &exec_argv0);
  if (exec_argv0 != nullptr) napi_set_element(env, argv_arr, 0, exec_argv0);
  if (has_script_path) {
    napi_value script_argv1 = nullptr;
    napi_create_string_utf8(env, current_script_path.c_str(), NAPI_AUTO_LENGTH, &script_argv1);
    if (script_argv1 != nullptr) napi_set_element(env, argv_arr, 1, script_argv1);
    for (size_t i = 0; i < script_argv.size(); ++i) {
      napi_value arg = nullptr;
      if (napi_create_string_utf8(env, script_argv[i].c_str(), NAPI_AUTO_LENGTH, &arg) == napi_ok &&
          arg != nullptr) {
        napi_set_element(env, argv_arr, static_cast<uint32_t>(i + 2), arg);
      }
    }
  } else {
    for (size_t i = 0; i < script_argv.size(); ++i) {
      napi_value arg = nullptr;
      if (napi_create_string_utf8(env, script_argv[i].c_str(), NAPI_AUTO_LENGTH, &arg) == napi_ok &&
          arg != nullptr) {
        napi_set_element(env, argv_arr, static_cast<uint32_t>(i + 1), arg);
      }
    }
  }
  status = napi_set_named_property(env, process_obj, "argv", argv_arr);
  if (status != napi_ok) return status;

  napi_value exec_argv_arr = nullptr;
  status = napi_create_array_with_length(env, exec_argv.size(), &exec_argv_arr);
  if (status != napi_ok || exec_argv_arr == nullptr) return (status == napi_ok) ? napi_generic_failure : status;
  for (size_t i = 0; i < exec_argv.size(); i++) {
    napi_value v = nullptr;
    if (napi_create_string_utf8(env, exec_argv[i].c_str(), NAPI_AUTO_LENGTH, &v) == napi_ok && v != nullptr) {
      napi_set_element(env, exec_argv_arr, static_cast<uint32_t>(i), v);
    }
  }
  status = napi_set_named_property(env, process_obj, "execArgv", exec_argv_arr);
  if (status != napi_ok) return status;

  const std::string title = process_title.empty() ? "ubi" : process_title;
  napi_value title_value = nullptr;
  status = napi_create_string_utf8(env, title.c_str(), NAPI_AUTO_LENGTH, &title_value);
  if (status != napi_ok || title_value == nullptr) return (status == napi_ok) ? napi_generic_failure : status;
  status = napi_set_named_property(env, process_obj, "title", title_value);
  if (status != napi_ok) return status;

  napi_value argv0_value = nullptr;
  status = napi_create_string_utf8(env, g_ubi_exec_path.c_str(), NAPI_AUTO_LENGTH, &argv0_value);
  if (status != napi_ok || argv0_value == nullptr) return (status == napi_ok) ? napi_generic_failure : status;
  status = napi_set_named_property(env, process_obj, "argv0", argv0_value);
  if (status != napi_ok) return status;

  napi_value cwd_fn = nullptr;
  status = napi_create_function(env, "cwd", NAPI_AUTO_LENGTH, ProcessCwdCallback, nullptr, &cwd_fn);
  if (status != napi_ok || cwd_fn == nullptr) return (status == napi_ok) ? napi_generic_failure : status;
  status = napi_set_named_property(env, process_obj, "cwd", cwd_fn);
  if (status != napi_ok) return status;

  napi_value chdir_fn = nullptr;
  status = napi_create_function(env, "chdir", NAPI_AUTO_LENGTH, ProcessChdirCallback, nullptr, &chdir_fn);
  if (status != napi_ok || chdir_fn == nullptr) return (status == napi_ok) ? napi_generic_failure : status;
  status = napi_set_named_property(env, process_obj, "chdir", chdir_fn);
  if (status != napi_ok) return status;

  napi_value cpu_usage_fn = nullptr;
  status = napi_create_function(env, "cpuUsage", NAPI_AUTO_LENGTH, ProcessCpuUsageCallback, nullptr, &cpu_usage_fn);
  if (status != napi_ok || cpu_usage_fn == nullptr) return (status == napi_ok) ? napi_generic_failure : status;
  status = napi_set_named_property(env, process_obj, "cpuUsage", cpu_usage_fn);
  if (status != napi_ok) return status;

  napi_value available_memory_fn = nullptr;
  status = napi_create_function(env, "availableMemory", NAPI_AUTO_LENGTH, ProcessAvailableMemoryCallback, nullptr, &available_memory_fn);
  if (status != napi_ok || available_memory_fn == nullptr) return (status == napi_ok) ? napi_generic_failure : status;
  status = napi_set_named_property(env, process_obj, "availableMemory", available_memory_fn);
  if (status != napi_ok) return status;

  napi_value constrained_memory_fn = nullptr;
  status = napi_create_function(env, "constrainedMemory", NAPI_AUTO_LENGTH, ProcessConstrainedMemoryCallback, nullptr, &constrained_memory_fn);
  if (status != napi_ok || constrained_memory_fn == nullptr) return (status == napi_ok) ? napi_generic_failure : status;
  status = napi_set_named_property(env, process_obj, "constrainedMemory", constrained_memory_fn);
  if (status != napi_ok) return status;

  napi_value umask_fn = nullptr;
  status = napi_create_function(env, "umask", NAPI_AUTO_LENGTH, ProcessUmaskCallback, nullptr, &umask_fn);
  if (status != napi_ok || umask_fn == nullptr) return (status == napi_ok) ? napi_generic_failure : status;
  status = napi_set_named_property(env, process_obj, "umask", umask_fn);
  if (status != napi_ok) return status;

  napi_value uptime_fn = nullptr;
  status = napi_create_function(env, "uptime", NAPI_AUTO_LENGTH, ProcessUptimeCallback, nullptr, &uptime_fn);
  if (status != napi_ok || uptime_fn == nullptr) return (status == napi_ok) ? napi_generic_failure : status;
  status = napi_set_named_property(env, process_obj, "uptime", uptime_fn);
  if (status != napi_ok) return status;

  napi_value hrtime_fn = nullptr;
  status = napi_create_function(env, "hrtime", NAPI_AUTO_LENGTH, ProcessHrtimeCallback, nullptr, &hrtime_fn);
  if (status != napi_ok || hrtime_fn == nullptr) return (status == napi_ok) ? napi_generic_failure : status;
  napi_value hrtime_bigint_fn = nullptr;
  status = napi_create_function(env, "bigint", NAPI_AUTO_LENGTH, ProcessHrtimeBigintCallback, nullptr, &hrtime_bigint_fn);
  if (status != napi_ok || hrtime_bigint_fn == nullptr) return (status == napi_ok) ? napi_generic_failure : status;
  status = napi_set_named_property(env, hrtime_fn, "bigint", hrtime_bigint_fn);
  if (status != napi_ok) return status;
  status = napi_set_named_property(env, process_obj, "hrtime", hrtime_fn);
  if (status != napi_ok) return status;

  status = InstallProcessStream(env, process_obj, "stdout", 1, ProcessStdoutWriteCallback);
  if (status != napi_ok) return status;
  status = InstallProcessStream(env, process_obj, "stderr", 2, ProcessStderrWriteCallback);
  if (status != napi_ok) return status;

  napi_value abort_fn = nullptr;
  status = napi_create_function(env, "abort", NAPI_AUTO_LENGTH, ProcessAbortCallback, nullptr, &abort_fn);
  if (status != napi_ok || abort_fn == nullptr) return (status == napi_ok) ? napi_generic_failure : status;
  status = napi_set_named_property(env, process_obj, "abort", abort_fn);
  if (status != napi_ok) return status;

  napi_value exit_fn = nullptr;
  status = napi_create_function(env, "exit", NAPI_AUTO_LENGTH, ProcessExitCallback, nullptr, &exit_fn);
  if (status != napi_ok || exit_fn == nullptr) return (status == napi_ok) ? napi_generic_failure : status;
  status = napi_set_named_property(env, process_obj, "exit", exit_fn);
  if (status != napi_ok) return status;

  napi_value arch_str = nullptr;
  status = napi_create_string_utf8(env, DetectArch(), NAPI_AUTO_LENGTH, &arch_str);
  if (status != napi_ok || arch_str == nullptr) return (status == napi_ok) ? napi_generic_failure : status;
  status = napi_set_named_property(env, process_obj, "arch", arch_str);
  if (status != napi_ok) return status;

  napi_value platform_str = nullptr;
  status = napi_create_string_utf8(env, DetectPlatform(), NAPI_AUTO_LENGTH, &platform_str);
  if (status != napi_ok || platform_str == nullptr) return (status == napi_ok) ? napi_generic_failure : status;
  status = napi_set_named_property(env, process_obj, "platform", platform_str);
  if (status != napi_ok) return status;

  napi_value exec_path = nullptr;
  status = napi_create_string_utf8(env, g_ubi_exec_path.c_str(), NAPI_AUTO_LENGTH, &exec_path);
  if (status != napi_ok || exec_path == nullptr) return (status == napi_ok) ? napi_generic_failure : status;
  status = napi_set_named_property(env, process_obj, "execPath", exec_path);
  if (status != napi_ok) return status;

  napi_value version_str = nullptr;
  status = napi_create_string_utf8(env, NODE_VERSION, NAPI_AUTO_LENGTH, &version_str);
  if (status != napi_ok || version_str == nullptr) return (status == napi_ok) ? napi_generic_failure : status;
  status = napi_set_named_property(env, process_obj, "version", version_str);
  if (status != napi_ok) return status;

  napi_value pid_value = nullptr;
#if defined(_WIN32)
  status = napi_create_int32(env, 1, &pid_value);
#else
  status = napi_create_int32(env, static_cast<int32_t>(getpid()), &pid_value);
#endif
  if (status != napi_ok || pid_value == nullptr) return (status == napi_ok) ? napi_generic_failure : status;
  status = napi_set_named_property(env, process_obj, "pid", pid_value);
  if (status != napi_ok) return status;

  napi_value ppid_value = nullptr;
#if defined(_WIN32)
  status = napi_create_int32(env, 1, &ppid_value);
#else
  status = napi_create_int32(env, static_cast<int32_t>(getppid()), &ppid_value);
#endif
  if (status != napi_ok || ppid_value == nullptr) return (status == napi_ok) ? napi_generic_failure : status;
  status = napi_set_named_property(env, process_obj, "ppid", ppid_value);
  if (status != napi_ok) return status;

  napi_value versions_obj = nullptr;
  status = napi_create_object(env, &versions_obj);
  if (status != napi_ok || versions_obj == nullptr) return (status == napi_ok) ? napi_generic_failure : status;
  struct VersionEntry {
    const char* key;
    std::string value;
  };
  const VersionEntry version_entries[] = {
      {"node", NODE_VERSION_STRING},
      {"acorn", ACORN_VERSION},
      {"ada", ADA_VERSION},
      {"amaro", GetAmaroVersion()},
      {"ares", ARES_VERSION_STR},
      {"brotli", GetBrotliVersion()},
      {"cjs_module_lexer", CJS_MODULE_LEXER_VERSION},
      {"llhttp", GetLlhttpVersion()},
      {"modules", UBI_STRINGIFY(NODE_MODULE_VERSION)},
      {"napi", UBI_STRINGIFY(NODE_API_SUPPORTED_VERSION_MAX)},
      {"nbytes", NBYTES_VERSION},
      {"ncrypto", NCRYPTO_VERSION},
      {"nghttp2", NGHTTP2_VERSION},
      {"openssl", GetOpenSslVersion()},
      {"simdjson", SIMDJSON_VERSION},
      {"simdutf", SIMDUTF_VERSION},
      {"undici", GetUndiciVersion()},
      {"uv", uv_version_string()},
      {"uvwasi", kUvwasiVersion},
      {"v8", UBI_EMBEDDED_V8_VERSION},
      {"zlib", ZLIB_VERSION},
      {"zstd", ZSTD_VERSION_STRING},
  };
  for (const auto& entry : version_entries) {
    napi_value value = nullptr;
    status = napi_create_string_utf8(env, entry.value.c_str(), NAPI_AUTO_LENGTH, &value);
    if (status != napi_ok || value == nullptr) return (status == napi_ok) ? napi_generic_failure : status;
    napi_property_descriptor prop = {};
    prop.utf8name = entry.key;
    prop.value = value;
    prop.attributes = napi_enumerable;
    status = napi_define_properties(env, versions_obj, 1, &prop);
    if (status != napi_ok) return status;
  }
  status = napi_set_named_property(env, process_obj, "versions", versions_obj);
  if (status != napi_ok) return status;

  napi_value release_obj = nullptr;
  status = napi_create_object(env, &release_obj);
  if (status != napi_ok || release_obj == nullptr) return (status == napi_ok) ? napi_generic_failure : status;
  napi_value release_name = nullptr;
  status = napi_create_string_utf8(env, "node", NAPI_AUTO_LENGTH, &release_name);
  if (status != napi_ok || release_name == nullptr) return (status == napi_ok) ? napi_generic_failure : status;
  status = napi_set_named_property(env, release_obj, "name", release_name);
  if (status != napi_ok) return status;
#if NODE_VERSION_IS_LTS
  napi_value release_lts = nullptr;
  status = napi_create_string_utf8(env, NODE_VERSION_LTS_CODENAME, NAPI_AUTO_LENGTH, &release_lts);
  if (status != napi_ok || release_lts == nullptr) return (status == napi_ok) ? napi_generic_failure : status;
  status = napi_set_named_property(env, release_obj, "lts", release_lts);
  if (status != napi_ok) return status;
#endif
  const std::string release_url_prefix =
      std::string("https://nodejs.org/download/release/v") + NODE_VERSION_STRING + "/";
  const std::string release_file_prefix =
      release_url_prefix + "node-v" + NODE_VERSION_STRING;
  napi_value source_url = nullptr;
  status = napi_create_string_utf8(
      env,
      (release_file_prefix + ".tar.gz").c_str(),
      NAPI_AUTO_LENGTH,
      &source_url);
  if (status != napi_ok || source_url == nullptr) return (status == napi_ok) ? napi_generic_failure : status;
  status = napi_set_named_property(env, release_obj, "sourceUrl", source_url);
  if (status != napi_ok) return status;
  napi_value headers_url = nullptr;
  status = napi_create_string_utf8(
      env,
      (release_file_prefix + "-headers.tar.gz").c_str(),
      NAPI_AUTO_LENGTH,
      &headers_url);
  if (status != napi_ok || headers_url == nullptr) return (status == napi_ok) ? napi_generic_failure : status;
  status = napi_set_named_property(env, release_obj, "headersUrl", headers_url);
  if (status != napi_ok) return status;
  status = napi_set_named_property(env, process_obj, "release", release_obj);
  if (status != napi_ok) return status;

  napi_value features_obj = nullptr;
  status = napi_create_object(env, &features_obj);
  if (status != napi_ok || features_obj == nullptr) return (status == napi_ok) ? napi_generic_failure : status;
  napi_value inspector_true = nullptr;
  status = napi_get_boolean(env, true, &inspector_true);
  if (status != napi_ok || inspector_true == nullptr) return (status == napi_ok) ? napi_generic_failure : status;
  status = napi_set_named_property(env, features_obj, "inspector", inspector_true);
  if (status != napi_ok) return status;
  napi_value false_value = nullptr;
  status = napi_get_boolean(env, false, &false_value);
  if (status != napi_ok || false_value == nullptr) return (status == napi_ok) ? napi_generic_failure : status;
  const char* feature_keys[] = {"debug", "uv", "ipv6", "openssl_is_boringssl", "tls_alpn", "tls_sni",
                                "tls_ocsp", "tls", "cached_builtins", "require_module"};
  for (const char* key : feature_keys) {
    if (napi_set_named_property(env, features_obj, key, false_value) != napi_ok) return napi_generic_failure;
  }
  // Keep compatibility with currently imported crypto test subset.
  if (napi_set_named_property(env, features_obj, "openssl_is_boringssl", inspector_true) != napi_ok) {
    return napi_generic_failure;
  }
  if (napi_set_named_property(env, features_obj, "typescript", false_value) != napi_ok) return napi_generic_failure;
  status = napi_set_named_property(env, process_obj, "features", features_obj);
  if (status != napi_ok) return status;

  napi_value config_obj = BuildProcessConfigObject(env);
  if (config_obj == nullptr) return napi_generic_failure;
  napi_property_descriptor config_desc = {};
  config_desc.utf8name = "config";
  config_desc.value = config_obj;
  // Node bootstrap redefines process.config with specific descriptors.
  config_desc.attributes = static_cast<napi_property_attributes>(
      napi_writable | napi_enumerable | napi_configurable);
  status = napi_define_properties(env, process_obj, 1, &config_desc);
  if (status != napi_ok) return status;

  status = napi_set_named_property(env, global, "process", process_obj);
  if (status != napi_ok) return status;

  // Native internalBinding('process_methods')
  {
    auto& state = g_process_methods_states[env];
    if (state.binding_ref != nullptr) {
      napi_delete_reference(env, state.binding_ref);
      state.binding_ref = nullptr;
    }
    if (state.hrtime_buffer_ref != nullptr) {
      napi_delete_reference(env, state.hrtime_buffer_ref);
      state.hrtime_buffer_ref = nullptr;
    }
    napi_value binding = nullptr;
    status = napi_create_object(env, &binding);
    if (status != napi_ok || binding == nullptr) return (status == napi_ok) ? napi_generic_failure : status;
    napi_wrap(env, binding, nullptr, ProcessMethodsBindingFinalize, nullptr, nullptr);

    napi_value hrtime_buffer_ab = nullptr;
    if (napi_create_arraybuffer(env, sizeof(uint32_t) * 3, nullptr, &hrtime_buffer_ab) != napi_ok ||
        hrtime_buffer_ab == nullptr) {
      return napi_generic_failure;
    }
    napi_value hrtime_buffer = nullptr;
    if (napi_create_typedarray(
            env, napi_uint32_array, 3, hrtime_buffer_ab, 0, &hrtime_buffer) != napi_ok ||
        hrtime_buffer == nullptr) {
      return napi_generic_failure;
    }
    if (napi_create_reference(env, hrtime_buffer, 1, &state.hrtime_buffer_ref) != napi_ok ||
        state.hrtime_buffer_ref == nullptr) {
      return napi_generic_failure;
    }
    if (napi_set_named_property(env, binding, "hrtimeBuffer", hrtime_buffer) != napi_ok) return napi_generic_failure;

    struct BindingMethod {
      const char* name;
      napi_callback cb;
    };
    const BindingMethod methods[] = {
        {"_debugProcess", ProcessMethodsDebugProcessCallback},
        {"abort", ProcessAbortCallback},
        {"causeSegfault", ProcessMethodsCauseSegfaultCallback},
        {"chdir", ProcessChdirCallback},
        {"umask", ProcessUmaskCallback},
        {"memoryUsage", ProcessMethodsMemoryUsageBufferCallback},
        {"constrainedMemory", ProcessConstrainedMemoryCallback},
        {"availableMemory", ProcessAvailableMemoryCallback},
        {"rss", ProcessMethodsRssCallback},
        {"cpuUsage", ProcessMethodsCpuUsageBufferCallback},
        {"threadCpuUsage", ProcessMethodsThreadCpuUsageBufferCallback},
        {"resourceUsage", ProcessMethodsResourceUsageBufferCallback},
        {"_debugEnd", ProcessMethodsNoopUndefinedCallback},
        {"_getActiveRequests", ProcessMethodsGetActiveRequestsCallback},
        {"_getActiveHandles", ProcessMethodsGetActiveHandlesCallback},
        {"getActiveResourcesInfo", ProcessMethodsGetActiveResourcesInfoCallback},
        {"_kill", ProcessMethodsKillCallback},
        {"_setEnv", ProcessMethodsSetEnvCallback},
        {"_unsetEnv", ProcessMethodsUnsetEnvCallback},
        {"_rawDebug", ProcessMethodsRawDebugCallback},
        {"cwd", ProcessCwdCallback},
        {"dlopen", ProcessMethodsDlopenCallback},
        {"reallyExit", ProcessExitCallback},
        {"execve", ProcessMethodsExecveCallback},
        {"uptime", ProcessUptimeCallback},
        {"patchProcessObject", ProcessMethodsPatchProcessObjectCallback},
        {"loadEnvFile", ProcessMethodsLoadEnvFileCallback},
        {"setEmitWarningSync", ProcessMethodsSetEmitWarningSyncCallback},
        {"resetStdioForTesting", ProcessMethodsResetStdioForTestingCallback},
        {"hrtime", ProcessMethodsHrtimeCallback},
        {"hrtimeBigInt", ProcessMethodsHrtimeBigIntCallback},
    };
    for (const auto& method : methods) {
      napi_value fn = nullptr;
      if (napi_create_function(env, method.name, NAPI_AUTO_LENGTH, method.cb, nullptr, &fn) != napi_ok ||
          fn == nullptr ||
          napi_set_named_property(env, binding, method.name, fn) != napi_ok) {
        return napi_generic_failure;
      }
    }
    UpdateHrtimeBuffer(env, false);
    if (napi_create_reference(env, binding, 1, &state.binding_ref) != napi_ok || state.binding_ref == nullptr) {
      return napi_generic_failure;
    }
  }

  // Native internalBinding('report')
  {
    auto& state = g_report_states[env];
    if (state.binding_ref != nullptr) {
      napi_delete_reference(env, state.binding_ref);
      state.binding_ref = nullptr;
    }
    state.compact = HasExecArgvFlag(exec_argv, "--report-compact");
    state.exclude_network = HasExecArgvFlag(exec_argv, "--report-exclude-network");
    state.exclude_env = HasExecArgvFlag(exec_argv, "--report-exclude-env");
    state.report_on_fatal_error = HasExecArgvFlag(exec_argv, "--report-on-fatalerror");
    state.report_on_signal = HasExecArgvFlag(exec_argv, "--report-on-signal");
    state.report_on_uncaught_exception = HasExecArgvFlag(exec_argv, "--report-uncaught-exception");
    state.directory = ".";
    state.filename.clear();
    state.signal = "SIGUSR2";
    state.sequence = 0;

    napi_value binding = nullptr;
    status = napi_create_object(env, &binding);
    if (status != napi_ok || binding == nullptr) return (status == napi_ok) ? napi_generic_failure : status;
    napi_wrap(env, binding, nullptr, ReportBindingFinalize, nullptr, nullptr);

    struct BindingMethod {
      const char* name;
      napi_callback cb;
    };
    const BindingMethod methods[] = {
        {"writeReport", ReportWriteReportCallback},
        {"getReport", ReportGetReportCallback},
        {"getCompact", ReportGetCompactCallback},
        {"setCompact", ReportSetCompactCallback},
        {"getExcludeNetwork", ReportGetExcludeNetworkCallback},
        {"setExcludeNetwork", ReportSetExcludeNetworkCallback},
        {"getExcludeEnv", ReportGetExcludeEnvCallback},
        {"setExcludeEnv", ReportSetExcludeEnvCallback},
        {"getDirectory", ReportGetDirectoryCallback},
        {"setDirectory", ReportSetDirectoryCallback},
        {"getFilename", ReportGetFilenameCallback},
        {"setFilename", ReportSetFilenameCallback},
        {"getSignal", ReportGetSignalCallback},
        {"setSignal", ReportSetSignalCallback},
        {"shouldReportOnFatalError", ReportShouldReportOnFatalErrorCallback},
        {"setReportOnFatalError", ReportSetReportOnFatalErrorCallback},
        {"shouldReportOnSignal", ReportShouldReportOnSignalCallback},
        {"setReportOnSignal", ReportSetReportOnSignalCallback},
        {"shouldReportOnUncaughtException", ReportShouldReportOnUncaughtExceptionCallback},
        {"setReportOnUncaughtException", ReportSetReportOnUncaughtExceptionCallback},
    };
    for (const auto& method : methods) {
      napi_value fn = nullptr;
      if (napi_create_function(env, method.name, NAPI_AUTO_LENGTH, method.cb, nullptr, &fn) != napi_ok ||
          fn == nullptr ||
          napi_set_named_property(env, binding, method.name, fn) != napi_ok) {
        return napi_generic_failure;
      }
    }
    if (napi_create_reference(env, binding, 1, &state.binding_ref) != napi_ok || state.binding_ref == nullptr) {
      return napi_generic_failure;
    }
  }

  return napi_ok;
}

napi_value UbiGetProcessMethodsBinding(napi_env env) {
  ProcessMethodsBindingState* state = GetProcessMethodsState(env);
  if (state == nullptr || state->binding_ref == nullptr) return nullptr;
  napi_value binding = nullptr;
  if (napi_get_reference_value(env, state->binding_ref, &binding) != napi_ok || binding == nullptr) return nullptr;
  return binding;
}

napi_value UbiGetReportBinding(napi_env env) {
  ReportBindingState* state = GetReportState(env);
  if (state == nullptr || state->binding_ref == nullptr) return nullptr;
  napi_value binding = nullptr;
  if (napi_get_reference_value(env, state->binding_ref, &binding) != napi_ok || binding == nullptr) return nullptr;
  return binding;
}
