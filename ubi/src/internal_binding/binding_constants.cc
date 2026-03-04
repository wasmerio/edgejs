#include "internal_binding/dispatch.h"

#include "internal_binding/helpers.h"

namespace internal_binding {

namespace {

bool IsObjectLike(napi_env env, napi_value value) {
  if (value == nullptr) return false;
  napi_valuetype type = napi_undefined;
  if (napi_typeof(env, value, &type) != napi_ok) return false;
  return type == napi_object || type == napi_function;
}

napi_value GetNamedProperty(napi_env env, napi_value target, const char* key) {
  if (target == nullptr) return nullptr;
  napi_value out = nullptr;
  if (napi_get_named_property(env, target, key, &out) != napi_ok || out == nullptr) return nullptr;
  return out;
}

napi_value CreateNullPrototypeObject(napi_env env) {
  const napi_value undefined = Undefined(env);
  const napi_value global = GetGlobal(env);
  if (global == nullptr) return undefined;

  napi_value object_ctor = nullptr;
  if (napi_get_named_property(env, global, "Object", &object_ctor) != napi_ok ||
      object_ctor == nullptr || !IsObjectLike(env, object_ctor)) {
    return undefined;
  }

  napi_value create_fn = nullptr;
  if (napi_get_named_property(env, object_ctor, "create", &create_fn) != napi_ok ||
      create_fn == nullptr) {
    return undefined;
  }
  napi_valuetype fn_type = napi_undefined;
  if (napi_typeof(env, create_fn, &fn_type) != napi_ok || fn_type != napi_function) {
    return undefined;
  }

  napi_value null_value = nullptr;
  if (napi_get_null(env, &null_value) != napi_ok || null_value == nullptr) return undefined;

  napi_value out = nullptr;
  napi_value argv[1] = {null_value};
  if (napi_call_function(env, object_ctor, create_fn, 1, argv, &out) != napi_ok || out == nullptr) {
    bool pending = false;
    if (napi_is_exception_pending(env, &pending) == napi_ok && pending) {
      napi_value ignored = nullptr;
      napi_get_and_clear_last_exception(env, &ignored);
    }
    return undefined;
  }
  return out;
}

napi_value CreatePlainObject(napi_env env) {
  napi_value out = nullptr;
  if (napi_create_object(env, &out) != napi_ok || out == nullptr) return Undefined(env);
  return out;
}

napi_value CreateBestEffortNullProtoObject(napi_env env) {
  napi_value out = CreateNullPrototypeObject(env);
  if (out == nullptr || IsUndefined(env, out) || !IsObjectLike(env, out)) {
    out = CreatePlainObject(env);
  }
  return out;
}

napi_value EnsureObjectProperty(napi_env env, napi_value target, const char* key) {
  napi_value current = GetNamedProperty(env, target, key);
  if (!IsObjectLike(env, current)) {
    current = CreatePlainObject(env);
    if (IsObjectLike(env, current)) napi_set_named_property(env, target, key, current);
  }
  return current;
}

void EnsureInt32Default(napi_env env, napi_value target, const char* key, int32_t value) {
  if (!IsObjectLike(env, target)) return;
  bool has_key = false;
  if (napi_has_named_property(env, target, key, &has_key) != napi_ok || has_key) return;
  SetInt32(env, target, key, value);
}

void CopyOwnProperties(napi_env env, napi_value src, napi_value dst) {
  if (!IsObjectLike(env, src) || !IsObjectLike(env, dst)) return;
  napi_value keys = nullptr;
  if (napi_get_property_names(env, src, &keys) != napi_ok || keys == nullptr) return;
  uint32_t key_count = 0;
  if (napi_get_array_length(env, keys, &key_count) != napi_ok) return;
  for (uint32_t i = 0; i < key_count; i++) {
    napi_value key = nullptr;
    if (napi_get_element(env, keys, i, &key) != napi_ok || key == nullptr) continue;
    napi_value value = nullptr;
    if (napi_get_property(env, src, key, &value) != napi_ok || value == nullptr) continue;
    napi_set_property(env, dst, key, value);
  }
}

napi_value TryRequireModule(napi_env env, const char* module_name) {
  const napi_value undefined = Undefined(env);
  const napi_value global = GetGlobal(env);
  if (global == nullptr) return undefined;

  napi_value require_fn = nullptr;
  if (napi_get_named_property(env, global, "require", &require_fn) != napi_ok || require_fn == nullptr) {
    return undefined;
  }
  napi_valuetype require_type = napi_undefined;
  if (napi_typeof(env, require_fn, &require_type) != napi_ok || require_type != napi_function) {
    return undefined;
  }

  napi_value module_name_v = nullptr;
  if (napi_create_string_utf8(env, module_name, NAPI_AUTO_LENGTH, &module_name_v) != napi_ok ||
      module_name_v == nullptr) {
    return undefined;
  }

  napi_value argv[1] = {module_name_v};
  napi_value module = nullptr;
  if (napi_call_function(env, global, require_fn, 1, argv, &module) != napi_ok || module == nullptr) {
    bool pending = false;
    if (napi_is_exception_pending(env, &pending) == napi_ok && pending) {
      napi_value ignored = nullptr;
      napi_get_and_clear_last_exception(env, &ignored);
    }
    return undefined;
  }
  return module;
}

napi_value TryGetModuleConstants(napi_env env, const char* module_name) {
  const napi_value undefined = Undefined(env);
  const napi_value module = TryRequireModule(env, module_name);
  if (IsUndefined(env, module) || !IsObjectLike(env, module)) return undefined;

  napi_value constants = nullptr;
  if (napi_get_named_property(env, module, "constants", &constants) != napi_ok ||
      constants == nullptr || !IsObjectLike(env, constants)) {
    return undefined;
  }
  return constants;
}

bool CopyNumericOwnProperties(napi_env env, napi_value src, napi_value dst) {
  napi_value keys = nullptr;
  if (napi_get_property_names(env, src, &keys) != napi_ok || keys == nullptr) return false;
  uint32_t key_count = 0;
  if (napi_get_array_length(env, keys, &key_count) != napi_ok) return false;
  for (uint32_t i = 0; i < key_count; i++) {
    napi_value key = nullptr;
    if (napi_get_element(env, keys, i, &key) != napi_ok || key == nullptr) continue;

    napi_value value = nullptr;
    if (napi_get_property(env, src, key, &value) != napi_ok || value == nullptr) continue;

    napi_valuetype t = napi_undefined;
    if (napi_typeof(env, value, &t) != napi_ok || t != napi_number) continue;

    napi_set_property(env, dst, key, value);
  }
  return true;
}

napi_value CreateInternalConstants(napi_env env) {
  napi_value internal = nullptr;
  if (napi_create_object(env, &internal) != napi_ok || internal == nullptr) return Undefined(env);
  SetInt32(env, internal, "EXTENSIONLESS_FORMAT_JAVASCRIPT", 0);
  SetInt32(env, internal, "EXTENSIONLESS_FORMAT_WASM", 1);
  return internal;
}

napi_value CreateTraceConstants(napi_env env) {
  napi_value trace = nullptr;
  if (napi_create_object(env, &trace) != napi_ok || trace == nullptr) return Undefined(env);
  SetInt32(env, trace, "TRACE_EVENT_PHASE_NESTABLE_ASYNC_BEGIN", 'b');
  SetInt32(env, trace, "TRACE_EVENT_PHASE_NESTABLE_ASYNC_END", 'e');
  return trace;
}

napi_value CreateDefaultFsConstants(napi_env env) {
  napi_value fs_obj = CreatePlainObject(env);
  if (!IsObjectLike(env, fs_obj)) return Undefined(env);
  SetInt32(env, fs_obj, "F_OK", 0);
  SetInt32(env, fs_obj, "R_OK", 4);
  SetInt32(env, fs_obj, "W_OK", 2);
  SetInt32(env, fs_obj, "X_OK", 1);
  return fs_obj;
}

napi_value CreateEmptyObject(napi_env env) {
  return CreatePlainObject(env);
}

napi_value CreateDefaultOsConstants(napi_env env) {
  napi_value os_obj = CreatePlainObject(env);
  if (!IsObjectLike(env, os_obj)) return Undefined(env);
  napi_value signals = CreateBestEffortNullProtoObject(env);
  if (IsObjectLike(env, signals)) {
    napi_set_named_property(env, os_obj, "signals", signals);
  }
  return os_obj;
}

void SetNamedObjectIfValid(napi_env env, napi_value target, const char* key, napi_value value) {
  if (value != nullptr && !IsUndefined(env, value) && IsObjectLike(env, value)) {
    napi_set_named_property(env, target, key, value);
  }
}

void NormalizeConstantsShape(napi_env env, napi_value constants) {
  if (!IsObjectLike(env, constants)) return;

  // Ensure fs access constants are always available.
  napi_value fs_obj = EnsureObjectProperty(env, constants, "fs");
  EnsureInt32Default(env, fs_obj, "F_OK", 0);
  EnsureInt32Default(env, fs_obj, "R_OK", 4);
  EnsureInt32Default(env, fs_obj, "W_OK", 2);
  EnsureInt32Default(env, fs_obj, "X_OK", 1);

  // Keep os.signals as a clean map-like object.
  napi_value os_obj = EnsureObjectProperty(env, constants, "os");
  napi_value src_signals = GetNamedProperty(env, os_obj, "signals");
  if (!IsObjectLike(env, src_signals)) src_signals = CreatePlainObject(env);

  napi_value normalized_signals = CreateBestEffortNullProtoObject(env);
  if (!IsObjectLike(env, normalized_signals)) return;
  CopyOwnProperties(env, src_signals, normalized_signals);
  napi_set_named_property(env, os_obj, "signals", normalized_signals);
}

}  // namespace

napi_value ResolveConstants(napi_env env, const ResolveOptions& options) {
  const napi_value undefined = Undefined(env);
  napi_value out = nullptr;
  if (napi_create_object(env, &out) != napi_ok || out == nullptr) return undefined;

  SetNamedObjectIfValid(env, out, "os", CreateDefaultOsConstants(env));
  SetNamedObjectIfValid(env, out, "fs", CreateDefaultFsConstants(env));
  SetNamedObjectIfValid(env, out, "crypto", CreateEmptyObject(env));
  SetNamedObjectIfValid(env, out, "zlib", CreateEmptyObject(env));

  // Prefer native ubi constants when present.
  napi_value os_constants = nullptr;
  if (options.callbacks.resolve_binding != nullptr) {
    os_constants = options.callbacks.resolve_binding(env, options.state, "os_constants");
  }
  SetNamedObjectIfValid(env, out, "os", os_constants);

  napi_value fs_binding = nullptr;
  if (options.callbacks.resolve_binding != nullptr) {
    fs_binding = options.callbacks.resolve_binding(env, options.state, "fs");
  }
  if (!IsUndefined(env, fs_binding) && IsObjectLike(env, fs_binding)) {
    napi_value fs_constants_obj = nullptr;
    if (napi_create_object(env, &fs_constants_obj) == napi_ok && fs_constants_obj != nullptr) {
      CopyNumericOwnProperties(env, fs_binding, fs_constants_obj);
      SetNamedObjectIfValid(env, out, "fs", fs_constants_obj);
    }
  }

  // Fill high-impact constant surfaces from public module constants when available.
  SetNamedObjectIfValid(env, out, "zlib", TryGetModuleConstants(env, "zlib"));
  SetNamedObjectIfValid(env, out, "crypto", TryGetModuleConstants(env, "crypto"));

  napi_value os_module_constants = TryGetModuleConstants(env, "os");
  if (!IsUndefined(env, os_module_constants) && IsObjectLike(env, os_module_constants)) {
    SetNamedObjectIfValid(env, out, "os", os_module_constants);
  }
  napi_value fs_module_constants = TryGetModuleConstants(env, "fs");
  if (!IsUndefined(env, fs_module_constants) && IsObjectLike(env, fs_module_constants)) {
    SetNamedObjectIfValid(env, out, "fs", fs_module_constants);
  }

  SetNamedObjectIfValid(env, out, "internal", CreateInternalConstants(env));
  SetNamedObjectIfValid(env, out, "trace", CreateTraceConstants(env));
  NormalizeConstantsShape(env, out);

  return out;
}

}  // namespace internal_binding
