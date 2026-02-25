#include "unode_node_api.h"

#include <iostream>
#include <string>

namespace {

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

napi_value ConsoleLogCallback(napi_env env, napi_callback_info info) {
  size_t argc = 8;
  napi_value args[8] = {nullptr};
  napi_status status = napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
  if (status == napi_ok) {
    for (size_t i = 0; i < argc; ++i) {
      if (i > 0) {
        std::cout << " ";
      }
      std::cout << ValueToUtf8(env, args[i]);
    }
    std::cout << "\n";
  }
  napi_value undefined = nullptr;
  napi_get_undefined(env, &undefined);
  return undefined;
}

}  // namespace

napi_status UnodeInstallConsole(napi_env env) {
  if (env == nullptr) {
    return napi_invalid_arg;
  }

  napi_value global = nullptr;
  napi_status status = napi_get_global(env, &global);
  if (status != napi_ok || global == nullptr) {
    return (status == napi_ok) ? napi_generic_failure : status;
  }

  napi_value console_obj = nullptr;
  bool has_console = false;
  status = napi_has_named_property(env, global, "console", &has_console);
  if (status != napi_ok) return status;
  if (has_console) {
    status = napi_get_named_property(env, global, "console", &console_obj);
    if (status != napi_ok || console_obj == nullptr) {
      return (status == napi_ok) ? napi_generic_failure : status;
    }
  } else {
    status = napi_create_object(env, &console_obj);
    if (status != napi_ok || console_obj == nullptr) {
      return (status == napi_ok) ? napi_generic_failure : status;
    }
  }
  napi_value log_fn = nullptr;
  status = napi_create_function(env, "log", NAPI_AUTO_LENGTH, ConsoleLogCallback, nullptr, &log_fn);
  if (status != napi_ok || log_fn == nullptr) {
    return (status == napi_ok) ? napi_generic_failure : status;
  }
  status = napi_set_named_property(env, console_obj, "log", log_fn);
  if (status != napi_ok) {
    return status;
  }
  return napi_set_named_property(env, global, "console", console_obj);
}
