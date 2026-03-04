#include "internal_binding/dispatch.h"

#include <string>
#include <vector>

#include "internal_binding/helpers.h"

namespace internal_binding {

namespace {

void DefineMethod(napi_env env, napi_value obj, const char* name, napi_callback cb) {
  napi_value fn = nullptr;
  if (napi_create_function(env, name, NAPI_AUTO_LENGTH, cb, nullptr, &fn) == napi_ok && fn != nullptr) {
    napi_set_named_property(env, obj, name, fn);
  }
}

bool ValueToUtf8(napi_env env, napi_value value, std::string* out) {
  if (value == nullptr || out == nullptr) return false;
  size_t len = 0;
  if (napi_get_value_string_utf8(env, value, nullptr, 0, &len) != napi_ok) return false;
  std::string buffer(len + 1, '\0');
  size_t copied = 0;
  if (napi_get_value_string_utf8(env, value, buffer.data(), buffer.size(), &copied) != napi_ok) return false;
  buffer.resize(copied);
  *out = std::move(buffer);
  return true;
}

napi_value CreateSet(napi_env env) {
  napi_value global = nullptr;
  napi_value set_ctor = nullptr;
  if (napi_get_global(env, &global) != napi_ok || global == nullptr ||
      napi_get_named_property(env, global, "Set", &set_ctor) != napi_ok || set_ctor == nullptr) {
    return nullptr;
  }
  napi_valuetype t = napi_undefined;
  if (napi_typeof(env, set_ctor, &t) != napi_ok || t != napi_function) return nullptr;
  napi_value set_obj = nullptr;
  if (napi_new_instance(env, set_ctor, 0, nullptr, &set_obj) != napi_ok || set_obj == nullptr) {
    return nullptr;
  }
  return set_obj;
}

napi_value CloneArray(napi_env env, napi_value input) {
  bool is_array = false;
  if (input == nullptr || napi_is_array(env, input, &is_array) != napi_ok || !is_array) {
    napi_value empty = nullptr;
    napi_create_array_with_length(env, 0, &empty);
    return empty;
  }
  uint32_t len = 0;
  napi_get_array_length(env, input, &len);
  napi_value out = nullptr;
  if (napi_create_array_with_length(env, len, &out) != napi_ok || out == nullptr) return nullptr;
  for (uint32_t i = 0; i < len; ++i) {
    napi_value item = nullptr;
    if (napi_get_element(env, input, i, &item) == napi_ok && item != nullptr) {
      napi_set_element(env, out, i, item);
    }
  }
  return out;
}

napi_value BuiltinsGetCacheUsage(napi_env env, napi_callback_info info) {
  napi_value this_arg = nullptr;
  size_t argc = 0;
  napi_get_cb_info(env, info, &argc, nullptr, &this_arg, nullptr);

  napi_value out = nullptr;
  if (napi_create_object(env, &out) != napi_ok || out == nullptr) return Undefined(env);

  napi_value with_cache = CreateSet(env);
  if (with_cache != nullptr) napi_set_named_property(env, out, "compiledWithCache", with_cache);
  napi_value without_cache = CreateSet(env);
  if (without_cache != nullptr) napi_set_named_property(env, out, "compiledWithoutCache", without_cache);

  napi_value builtin_ids = nullptr;
  if (this_arg != nullptr &&
      napi_get_named_property(env, this_arg, "builtinIds", &builtin_ids) == napi_ok &&
      builtin_ids != nullptr) {
    napi_value snapshot = CloneArray(env, builtin_ids);
    if (snapshot != nullptr) napi_set_named_property(env, out, "compiledInSnapshot", snapshot);
  } else {
    napi_value empty = nullptr;
    napi_create_array_with_length(env, 0, &empty);
    if (empty != nullptr) napi_set_named_property(env, out, "compiledInSnapshot", empty);
  }

  return out;
}

void EnsureBuiltinCategories(napi_env env, napi_value binding) {
  bool has_categories = false;
  if (napi_has_named_property(env, binding, "builtinCategories", &has_categories) != napi_ok) return;
  if (has_categories) return;

  napi_value builtin_ids = nullptr;
  if (napi_get_named_property(env, binding, "builtinIds", &builtin_ids) != napi_ok || builtin_ids == nullptr) return;
  bool is_array = false;
  if (napi_is_array(env, builtin_ids, &is_array) != napi_ok || !is_array) return;

  uint32_t len = 0;
  napi_get_array_length(env, builtin_ids, &len);
  std::vector<napi_value> can_be_required;
  std::vector<napi_value> cannot_be_required;
  can_be_required.reserve(len);
  cannot_be_required.reserve(len);

  for (uint32_t i = 0; i < len; ++i) {
    napi_value id_value = nullptr;
    if (napi_get_element(env, builtin_ids, i, &id_value) != napi_ok || id_value == nullptr) continue;
    std::string id;
    if (!ValueToUtf8(env, id_value, &id)) continue;
    if (id.rfind("internal/", 0) == 0) {
      cannot_be_required.push_back(id_value);
    } else {
      can_be_required.push_back(id_value);
    }
  }

  napi_value categories = nullptr;
  if (napi_create_object(env, &categories) != napi_ok || categories == nullptr) return;

  napi_value can_array = nullptr;
  if (napi_create_array_with_length(env, can_be_required.size(), &can_array) == napi_ok && can_array != nullptr) {
    for (size_t i = 0; i < can_be_required.size(); ++i) {
      napi_set_element(env, can_array, static_cast<uint32_t>(i), can_be_required[i]);
    }
    napi_set_named_property(env, categories, "canBeRequired", can_array);
  }

  napi_value cannot_array = nullptr;
  if (napi_create_array_with_length(env, cannot_be_required.size(), &cannot_array) == napi_ok &&
      cannot_array != nullptr) {
    for (size_t i = 0; i < cannot_be_required.size(); ++i) {
      napi_set_element(env, cannot_array, static_cast<uint32_t>(i), cannot_be_required[i]);
    }
    napi_set_named_property(env, categories, "cannotBeRequired", cannot_array);
  }

  napi_set_named_property(env, binding, "builtinCategories", categories);
}

void EnsureNatives(napi_env env, napi_value binding) {
  bool has_natives = false;
  if (napi_has_named_property(env, binding, "natives", &has_natives) != napi_ok) return;
  if (has_natives) return;

  napi_value natives = nullptr;
  napi_value global = GetGlobal(env);
  napi_value process = nullptr;
  napi_value process_binding = nullptr;
  napi_value name = nullptr;
  if (global != nullptr &&
      napi_get_named_property(env, global, "process", &process) == napi_ok &&
      process != nullptr &&
      napi_get_named_property(env, process, "binding", &process_binding) == napi_ok &&
      process_binding != nullptr) {
    napi_valuetype binding_type = napi_undefined;
    if (napi_typeof(env, process_binding, &binding_type) == napi_ok && binding_type == napi_function &&
        napi_create_string_utf8(env, "natives", NAPI_AUTO_LENGTH, &name) == napi_ok && name != nullptr &&
        napi_call_function(env, process, process_binding, 1, &name, &natives) == napi_ok &&
        natives != nullptr) {
      // keep natives
    }
  }

  if (natives == nullptr) napi_create_object(env, &natives);
  if (natives != nullptr) napi_set_named_property(env, binding, "natives", natives);
}

void EnsureConfigsAlias(napi_env env, napi_value binding) {
  bool has_configs = false;
  if (napi_has_named_property(env, binding, "configs", &has_configs) != napi_ok || has_configs) return;
  napi_value config = nullptr;
  if (napi_get_named_property(env, binding, "config", &config) == napi_ok && config != nullptr) {
    napi_set_named_property(env, binding, "configs", config);
  }
}

void EnsureGetCacheUsage(napi_env env, napi_value binding) {
  bool has_method = false;
  if (napi_has_named_property(env, binding, "getCacheUsage", &has_method) != napi_ok) return;
  if (has_method) return;
  DefineMethod(env, binding, "getCacheUsage", BuiltinsGetCacheUsage);
}

}  // namespace

napi_value ResolveBuiltins(napi_env env, const ResolveOptions& options) {
  if (options.callbacks.get_or_create_builtins == nullptr) return Undefined(env);
  napi_value binding = options.callbacks.get_or_create_builtins(env, options.state);
  if (binding == nullptr || IsUndefined(env, binding)) return Undefined(env);
  EnsureBuiltinCategories(env, binding);
  EnsureNatives(env, binding);
  EnsureConfigsAlias(env, binding);
  EnsureGetCacheUsage(env, binding);
  return binding;
}

}  // namespace internal_binding
