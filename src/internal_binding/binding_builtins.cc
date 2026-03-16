#include "internal_binding/dispatch.h"
#include "builtin_catalog.h"

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

napi_value GetNamedPropertyIfPresent(napi_env env, napi_value object, const char* name) {
  bool has_property = false;
  if (object == nullptr || napi_has_named_property(env, object, name, &has_property) != napi_ok || !has_property) {
    return nullptr;
  }
  napi_value value = nullptr;
  if (napi_get_named_property(env, object, name, &value) != napi_ok) return nullptr;
  return value;
}

napi_value AppendArrayItemsToSet(napi_env env, napi_value set_obj, napi_value input) {
  bool is_array = false;
  if (set_obj == nullptr || input == nullptr || napi_is_array(env, input, &is_array) != napi_ok || !is_array) {
    return set_obj;
  }
  napi_value add_fn = GetNamedPropertyIfPresent(env, set_obj, "add");
  if (add_fn == nullptr) return set_obj;

  uint32_t len = 0;
  napi_get_array_length(env, input, &len);
  for (uint32_t i = 0; i < len; ++i) {
    napi_value item = nullptr;
    if (napi_get_element(env, input, i, &item) != napi_ok || item == nullptr) continue;
    napi_value ignored = nullptr;
    napi_call_function(env, set_obj, add_fn, 1, &item, &ignored);
  }
  return set_obj;
}

napi_value CreateStringArray(napi_env env, const std::vector<std::string>& values) {
  napi_value out = nullptr;
  if (napi_create_array_with_length(env, values.size(), &out) != napi_ok || out == nullptr) {
    return nullptr;
  }
  for (size_t i = 0; i < values.size(); ++i) {
    napi_value value = nullptr;
    if (napi_create_string_utf8(env, values[i].c_str(), NAPI_AUTO_LENGTH, &value) != napi_ok ||
        value == nullptr) {
      return nullptr;
    }
    if (napi_set_element(env, out, static_cast<uint32_t>(i), value) != napi_ok) {
      return nullptr;
    }
  }
  return out;
}

napi_value BuiltinsGetCacheUsage(napi_env env, napi_callback_info info) {
  (void)info;

  napi_value out = nullptr;
  if (napi_create_object(env, &out) != napi_ok || out == nullptr) return Undefined(env);

  napi_value with_cache = CreateSet(env);
  if (with_cache != nullptr) napi_set_named_property(env, out, "compiledWithCache", with_cache);
  napi_value without_cache = CreateSet(env);
  if (without_cache != nullptr) napi_set_named_property(env, out, "compiledWithoutCache", without_cache);

  const std::vector<std::string>& builtin_ids = builtin_catalog::AllBuiltinIds();
  napi_value builtin_ids_array = CreateStringArray(env, builtin_ids);
  if (builtin_ids_array != nullptr) {
    napi_set_named_property(env, out, "compiledInSnapshot", builtin_ids_array);
    AppendArrayItemsToSet(env, without_cache, builtin_ids_array);
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

  napi_value categories = nullptr;
  if (napi_create_object(env, &categories) != napi_ok || categories == nullptr) return;

  const builtin_catalog::BuiltinCategories& builtin_categories =
      builtin_catalog::GetBuiltinCategories();

  napi_value can_array = CreateStringArray(env, builtin_categories.can_be_required);
  if (can_array != nullptr) {
    napi_set_named_property(env, categories, "canBeRequired", can_array);
  }

  napi_value cannot_array = CreateStringArray(env, builtin_categories.cannot_be_required);
  if (cannot_array != nullptr) {
    napi_set_named_property(env, categories, "cannotBeRequired", cannot_array);
  }

  napi_set_named_property(env, binding, "builtinCategories", categories);
}

void EnsureNatives(napi_env env, napi_value binding) {
  bool has_natives = false;
  if (napi_has_named_property(env, binding, "natives", &has_natives) != napi_ok) return;
  if (has_natives) return;

  napi_value natives = nullptr;
  napi_create_object(env, &natives);
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
