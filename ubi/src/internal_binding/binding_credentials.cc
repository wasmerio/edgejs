#include "internal_binding/dispatch.h"

#include <cstdlib>
#include <string>
#include <vector>

#if !defined(_WIN32)
#include <grp.h>
#include <sys/types.h>
#include <unistd.h>
#endif

#include "internal_binding/helpers.h"

namespace internal_binding {

namespace {

std::string ValueToUtf8(napi_env env, napi_value value) {
  if (value == nullptr) return {};
  size_t len = 0;
  if (napi_get_value_string_utf8(env, value, nullptr, 0, &len) != napi_ok) return {};
  std::string out(len + 1, '\0');
  size_t copied = 0;
  if (napi_get_value_string_utf8(env, value, out.data(), out.size(), &copied) != napi_ok) return {};
  out.resize(copied);
  return out;
}

void ThrowFeatureUnavailable(napi_env env, const char* name) {
  const std::string msg = std::string(name) + " is not supported on this platform";
  napi_throw_error(env, "ERR_FEATURE_UNAVAILABLE_ON_PLATFORM", msg.c_str());
}

bool GetUint32Arg(napi_env env, napi_value value, uint32_t* out) {
  if (value == nullptr || out == nullptr) return false;
  return napi_get_value_uint32(env, value, out) == napi_ok;
}

napi_value CredentialsSafeGetenv(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  if (argc < 1 || argv[0] == nullptr) return Undefined(env);
  const std::string key = ValueToUtf8(env, argv[0]);
  if (key.empty()) return Undefined(env);
  const char* value = std::getenv(key.c_str());
  if (value == nullptr) return Undefined(env);
  napi_value out = nullptr;
  napi_create_string_utf8(env, value, NAPI_AUTO_LENGTH, &out);
  return out != nullptr ? out : Undefined(env);
}

napi_value CredentialsGetTempDir(napi_env env, napi_callback_info /*info*/) {
  const char* candidates[] = {"TMPDIR", "TMP", "TEMP"};
  const char* fallback =
#if defined(_WIN32)
      "C:\\Temp";
#else
      "/tmp";
#endif
  const char* value = nullptr;
  for (const char* key : candidates) {
    const char* candidate = std::getenv(key);
    if (candidate != nullptr && *candidate != '\0') {
      value = candidate;
      break;
    }
  }
  if (value == nullptr) value = fallback;
  napi_value out = nullptr;
  napi_create_string_utf8(env, value, NAPI_AUTO_LENGTH, &out);
  return out != nullptr ? out : Undefined(env);
}

napi_value CredentialsGetuid(napi_env env, napi_callback_info /*info*/) {
#if defined(_WIN32)
  napi_value out = nullptr;
  napi_create_uint32(env, 0, &out);
  return out != nullptr ? out : Undefined(env);
#else
  napi_value out = nullptr;
  napi_create_uint32(env, static_cast<uint32_t>(::getuid()), &out);
  return out != nullptr ? out : Undefined(env);
#endif
}

napi_value CredentialsGeteuid(napi_env env, napi_callback_info /*info*/) {
#if defined(_WIN32)
  napi_value out = nullptr;
  napi_create_uint32(env, 0, &out);
  return out != nullptr ? out : Undefined(env);
#else
  napi_value out = nullptr;
  napi_create_uint32(env, static_cast<uint32_t>(::geteuid()), &out);
  return out != nullptr ? out : Undefined(env);
#endif
}

napi_value CredentialsGetgid(napi_env env, napi_callback_info /*info*/) {
#if defined(_WIN32)
  napi_value out = nullptr;
  napi_create_uint32(env, 0, &out);
  return out != nullptr ? out : Undefined(env);
#else
  napi_value out = nullptr;
  napi_create_uint32(env, static_cast<uint32_t>(::getgid()), &out);
  return out != nullptr ? out : Undefined(env);
#endif
}

napi_value CredentialsGetegid(napi_env env, napi_callback_info /*info*/) {
#if defined(_WIN32)
  napi_value out = nullptr;
  napi_create_uint32(env, 0, &out);
  return out != nullptr ? out : Undefined(env);
#else
  napi_value out = nullptr;
  napi_create_uint32(env, static_cast<uint32_t>(::getegid()), &out);
  return out != nullptr ? out : Undefined(env);
#endif
}

napi_value CredentialsGetgroups(napi_env env, napi_callback_info /*info*/) {
  napi_value out = nullptr;
  napi_create_array_with_length(env, 0, &out);
  if (out == nullptr) return Undefined(env);

#if !defined(_WIN32)
  const int count = ::getgroups(0, nullptr);
  if (count > 0) {
    std::vector<gid_t> groups(static_cast<size_t>(count));
    if (::getgroups(count, groups.data()) >= 0) {
      for (size_t i = 0; i < groups.size(); ++i) {
        napi_value value = nullptr;
        if (napi_create_uint32(env, static_cast<uint32_t>(groups[i]), &value) == napi_ok && value != nullptr) {
          napi_set_element(env, out, static_cast<uint32_t>(i), value);
        }
      }
    }
  }
#endif
  return out;
}

napi_value CredentialsSetuid(napi_env env, napi_callback_info info) {
#if defined(_WIN32)
  ThrowFeatureUnavailable(env, "setuid");
  return nullptr;
#else
  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  uint32_t uid = 0;
  if (argc < 1 || !GetUint32Arg(env, argv[0], &uid)) {
    napi_throw_type_error(env, "ERR_INVALID_ARG_TYPE", "The \"uid\" argument must be a number.");
    return nullptr;
  }
  if (::setuid(static_cast<uid_t>(uid)) != 0) {
    napi_throw_error(env, "ERR_SYSTEM_ERROR", "setuid failed");
    return nullptr;
  }
  return Undefined(env);
#endif
}

napi_value CredentialsSeteuid(napi_env env, napi_callback_info info) {
#if defined(_WIN32)
  ThrowFeatureUnavailable(env, "seteuid");
  return nullptr;
#else
  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  uint32_t uid = 0;
  if (argc < 1 || !GetUint32Arg(env, argv[0], &uid)) {
    napi_throw_type_error(env, "ERR_INVALID_ARG_TYPE", "The \"uid\" argument must be a number.");
    return nullptr;
  }
  if (::seteuid(static_cast<uid_t>(uid)) != 0) {
    napi_throw_error(env, "ERR_SYSTEM_ERROR", "seteuid failed");
    return nullptr;
  }
  return Undefined(env);
#endif
}

napi_value CredentialsSetgid(napi_env env, napi_callback_info info) {
#if defined(_WIN32)
  ThrowFeatureUnavailable(env, "setgid");
  return nullptr;
#else
  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  uint32_t gid = 0;
  if (argc < 1 || !GetUint32Arg(env, argv[0], &gid)) {
    napi_throw_type_error(env, "ERR_INVALID_ARG_TYPE", "The \"gid\" argument must be a number.");
    return nullptr;
  }
  if (::setgid(static_cast<gid_t>(gid)) != 0) {
    napi_throw_error(env, "ERR_SYSTEM_ERROR", "setgid failed");
    return nullptr;
  }
  return Undefined(env);
#endif
}

napi_value CredentialsSetegid(napi_env env, napi_callback_info info) {
#if defined(_WIN32)
  ThrowFeatureUnavailable(env, "setegid");
  return nullptr;
#else
  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  uint32_t gid = 0;
  if (argc < 1 || !GetUint32Arg(env, argv[0], &gid)) {
    napi_throw_type_error(env, "ERR_INVALID_ARG_TYPE", "The \"gid\" argument must be a number.");
    return nullptr;
  }
  if (::setegid(static_cast<gid_t>(gid)) != 0) {
    napi_throw_error(env, "ERR_SYSTEM_ERROR", "setegid failed");
    return nullptr;
  }
  return Undefined(env);
#endif
}

napi_value CredentialsSetgroups(napi_env env, napi_callback_info info) {
#if defined(_WIN32)
  ThrowFeatureUnavailable(env, "setgroups");
  return nullptr;
#else
  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  if (argc < 1) {
    napi_throw_type_error(env, "ERR_INVALID_ARG_TYPE", "The \"groups\" argument must be an array.");
    return nullptr;
  }

  bool is_array = false;
  if (napi_is_array(env, argv[0], &is_array) != napi_ok || !is_array) {
    napi_throw_type_error(env, "ERR_INVALID_ARG_TYPE", "The \"groups\" argument must be an array.");
    return nullptr;
  }

  uint32_t len = 0;
  napi_get_array_length(env, argv[0], &len);
  std::vector<gid_t> groups(static_cast<size_t>(len));
  for (uint32_t i = 0; i < len; ++i) {
    napi_value element = nullptr;
    uint32_t gid = 0;
    if (napi_get_element(env, argv[0], i, &element) != napi_ok || !GetUint32Arg(env, element, &gid)) {
      napi_throw_type_error(env, "ERR_INVALID_ARG_TYPE", "Each group id must be a number.");
      return nullptr;
    }
    groups[i] = static_cast<gid_t>(gid);
  }

  if (::setgroups(groups.size(), groups.empty() ? nullptr : groups.data()) != 0) {
    napi_throw_error(env, "ERR_SYSTEM_ERROR", "setgroups failed");
    return nullptr;
  }
  return Undefined(env);
#endif
}

napi_value CredentialsInitgroups(napi_env env, napi_callback_info info) {
#if defined(_WIN32)
  ThrowFeatureUnavailable(env, "initgroups");
  return nullptr;
#else
  size_t argc = 2;
  napi_value argv[2] = {nullptr, nullptr};
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  if (argc < 2) {
    napi_throw_type_error(env,
                          "ERR_INVALID_ARG_TYPE",
                          "The \"user\" and \"extraGroup\" arguments are required.");
    return nullptr;
  }

  const std::string user = ValueToUtf8(env, argv[0]);
  uint32_t extra_group = 0;
  if (user.empty() || !GetUint32Arg(env, argv[1], &extra_group)) {
    napi_throw_type_error(env,
                          "ERR_INVALID_ARG_TYPE",
                          "The \"user\" argument must be string and \"extraGroup\" must be number.");
    return nullptr;
  }

  if (::initgroups(user.c_str(), static_cast<gid_t>(extra_group)) != 0) {
    napi_throw_error(env, "ERR_SYSTEM_ERROR", "initgroups failed");
    return nullptr;
  }
  return Undefined(env);
#endif
}

void DefineMethod(napi_env env, napi_value obj, const char* name, napi_callback cb) {
  napi_value fn = nullptr;
  if (napi_create_function(env, name, NAPI_AUTO_LENGTH, cb, nullptr, &fn) == napi_ok && fn != nullptr) {
    napi_set_named_property(env, obj, name, fn);
  }
}

}  // namespace

napi_value ResolveCredentials(napi_env env, const ResolveOptions& /*options*/) {
  napi_value out = nullptr;
  if (napi_create_object(env, &out) != napi_ok || out == nullptr) return Undefined(env);

  DefineMethod(env, out, "safeGetenv", CredentialsSafeGetenv);
  DefineMethod(env, out, "getTempDir", CredentialsGetTempDir);
  DefineMethod(env, out, "getuid", CredentialsGetuid);
  DefineMethod(env, out, "geteuid", CredentialsGeteuid);
  DefineMethod(env, out, "getgid", CredentialsGetgid);
  DefineMethod(env, out, "getegid", CredentialsGetegid);
  DefineMethod(env, out, "getgroups", CredentialsGetgroups);
  DefineMethod(env, out, "setuid", CredentialsSetuid);
  DefineMethod(env, out, "seteuid", CredentialsSeteuid);
  DefineMethod(env, out, "setgid", CredentialsSetgid);
  DefineMethod(env, out, "setegid", CredentialsSetegid);
  DefineMethod(env, out, "setgroups", CredentialsSetgroups);
  DefineMethod(env, out, "initgroups", CredentialsInitgroups);

#if defined(_WIN32)
  SetBool(env, out, "implementsPosixCredentials", false);
#else
  SetBool(env, out, "implementsPosixCredentials", true);
#endif

  return out;
}

}  // namespace internal_binding
