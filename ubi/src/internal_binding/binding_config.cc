#include "internal_binding/dispatch.h"

#include <cctype>
#include <cstdlib>
#include <string>

#include "internal_binding/helpers.h"

namespace internal_binding {

namespace {

napi_value GetNamed(napi_env env, napi_value obj, const char* key) {
  napi_value out = nullptr;
  if (obj == nullptr || napi_get_named_property(env, obj, key, &out) != napi_ok || out == nullptr) {
    return nullptr;
  }
  return out;
}

bool HasNamed(napi_env env, napi_value obj, const char* key) {
  if (obj == nullptr) return false;
  bool has_prop = false;
  return napi_has_named_property(env, obj, key, &has_prop) == napi_ok && has_prop;
}

bool IsTruthy(napi_env env, napi_value value) {
  if (value == nullptr) return false;
  napi_valuetype t = napi_undefined;
  if (napi_typeof(env, value, &t) != napi_ok) return false;
  if (t == napi_boolean) {
    bool out = false;
    if (napi_get_value_bool(env, value, &out) == napi_ok) return out;
  } else if (t == napi_number) {
    double n = 0;
    if (napi_get_value_double(env, value, &n) == napi_ok) return n != 0;
  } else if (t == napi_string) {
    size_t len = 0;
    if (napi_get_value_string_utf8(env, value, nullptr, 0, &len) != napi_ok || len == 0) return false;
    std::string text(len + 1, '\0');
    size_t copied = 0;
    if (napi_get_value_string_utf8(env, value, text.data(), text.size(), &copied) != napi_ok) return false;
    text.resize(copied);
    for (char& c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return text == "1" || text == "true" || text == "yes" || text == "on";
  }
  return false;
}

bool HasIntl(napi_env env) {
  napi_value global = GetGlobal(env);
  napi_value intl = GetNamed(env, global, "Intl");
  if (intl == nullptr) return false;
  napi_valuetype t = napi_undefined;
  return napi_typeof(env, intl, &t) == napi_ok && (t == napi_object || t == napi_function);
}

bool HasProcessNested(napi_env env, const char* first_key, const char* second_key) {
  napi_value global = GetGlobal(env);
  napi_value process = GetNamed(env, global, "process");
  if (process == nullptr) return false;
  napi_value first = GetNamed(env, process, first_key);
  if (first == nullptr) return false;
  return HasNamed(env, first, second_key);
}

bool GetProcessConfigVariable(napi_env env, const char* key) {
  napi_value global = GetGlobal(env);
  napi_value process = GetNamed(env, global, "process");
  napi_value config = GetNamed(env, process, "config");
  napi_value variables = GetNamed(env, config, "variables");
  napi_value value = GetNamed(env, variables, key);
  return IsTruthy(env, value);
}

napi_value ConfigGetDefaultLocale(napi_env env, napi_callback_info /*info*/) {
  const char* keys[] = {"LC_ALL", "LC_MESSAGES", "LANG"};
  for (const char* key : keys) {
    const char* value = std::getenv(key);
    if (value != nullptr && *value != '\0') {
      napi_value out = nullptr;
      napi_create_string_utf8(env, value, NAPI_AUTO_LENGTH, &out);
      return out != nullptr ? out : Undefined(env);
    }
  }
  return Undefined(env);
}

}  // namespace

napi_value ResolveConfig(napi_env env, const ResolveOptions& /*options*/) {
  napi_value out = nullptr;
  if (napi_create_object(env, &out) != napi_ok || out == nullptr) return Undefined(env);

  const bool has_intl = HasIntl(env);
  const bool has_inspector = HasProcessNested(env, "features", "inspector");
  const bool has_tracing = HasProcessNested(env, "features", "tracing");
  const bool has_openssl = HasProcessNested(env, "versions", "openssl");
  const bool openssl_is_boringssl =
      HasProcessNested(env, "versions", "boringssl") || HasProcessNested(env, "features", "openssl_is_boringssl");
  const bool has_small_icu = false;
  const bool fips_mode =
      GetProcessConfigVariable(env, "openssl_is_fips") || GetProcessConfigVariable(env, "node_fipsinstall");
  const bool is_debug_build = GetProcessConfigVariable(env, "debug");

  SetBool(env, out, "hasIntl", has_intl);
  SetBool(env, out, "hasSmallICU", has_small_icu);
  SetBool(env, out, "hasInspector", has_inspector);
  SetBool(env, out, "hasTracing", has_tracing);
  SetBool(env, out, "hasOpenSSL", has_openssl);
  SetBool(env, out, "openSSLIsBoringSSL", openssl_is_boringssl);
  SetBool(env, out, "fipsMode", fips_mode);
  SetBool(env, out, "hasNodeOptions", true);
  SetBool(env, out, "noBrowserGlobals", false);
  SetBool(env, out, "isDebugBuild", is_debug_build);
  SetInt32(env, out, "bits", static_cast<int32_t>(sizeof(void*) * 8));

  napi_value get_default_locale = nullptr;
  if (napi_create_function(env,
                           "getDefaultLocale",
                           NAPI_AUTO_LENGTH,
                           ConfigGetDefaultLocale,
                           nullptr,
                           &get_default_locale) == napi_ok &&
      get_default_locale != nullptr) {
    napi_set_named_property(env, out, "getDefaultLocale", get_default_locale);
  }

  return out;
}

}  // namespace internal_binding
