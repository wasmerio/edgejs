#include "internal_binding/dispatch.h"

#include <cstdint>

#include "internal_binding/helpers.h"

namespace internal_binding {

namespace {

enum class TypesAlias : uint8_t {
  kIsAnyArrayBuffer,
  kIsArrayBuffer,
  kIsAsyncFunction,
  kIsDataView,
  kIsDate,
  kIsExternal,
  kIsMap,
  kIsMapIterator,
  kIsNativeError,
  kIsPromise,
  kIsRegExp,
  kIsSet,
  kIsSetIterator,
};

const char* TypesMethodName(TypesAlias alias) {
  switch (alias) {
    case TypesAlias::kIsAnyArrayBuffer:
      return "isAnyArrayBuffer";
    case TypesAlias::kIsArrayBuffer:
      return "isArrayBuffer";
    case TypesAlias::kIsAsyncFunction:
      return "isAsyncFunction";
    case TypesAlias::kIsDataView:
      return "isDataView";
    case TypesAlias::kIsDate:
      return "isDate";
    case TypesAlias::kIsExternal:
      return "isExternal";
    case TypesAlias::kIsMap:
      return "isMap";
    case TypesAlias::kIsMapIterator:
      return "isMapIterator";
    case TypesAlias::kIsNativeError:
      return "isNativeError";
    case TypesAlias::kIsPromise:
      return "isPromise";
    case TypesAlias::kIsRegExp:
      return "isRegExp";
    case TypesAlias::kIsSet:
      return "isSet";
    case TypesAlias::kIsSetIterator:
      return "isSetIterator";
  }
  return "";
}

napi_value CallTypesPredicate(napi_env env, TypesAlias alias, napi_value value) {
  napi_value types = GetGlobalNamed(env, "__ubi_types");
  if (types == nullptr || IsUndefined(env, types)) return Undefined(env);
  const char* name = TypesMethodName(alias);
  napi_value fn = nullptr;
  napi_valuetype t = napi_undefined;
  if (napi_get_named_property(env, types, name, &fn) != napi_ok ||
      fn == nullptr ||
      napi_typeof(env, fn, &t) != napi_ok ||
      t != napi_function) {
    return Undefined(env);
  }
  napi_value out = nullptr;
  napi_value argv[1] = {value != nullptr ? value : Undefined(env)};
  if (napi_call_function(env, types, fn, 1, argv, &out) != napi_ok || out == nullptr) return Undefined(env);
  return out;
}

napi_value UtilTypesAliasCallback(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  void* data = nullptr;
  napi_get_cb_info(env, info, &argc, argv, nullptr, &data);
  TypesAlias alias = static_cast<TypesAlias>(reinterpret_cast<uintptr_t>(data));
  return CallTypesPredicate(env, alias, argc >= 1 ? argv[0] : Undefined(env));
}

napi_value UtilIsArrayBufferView(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  bool result = false;
  if (argc >= 1 && argv[0] != nullptr) {
    bool is_typedarray = false;
    bool is_dataview = false;
    if (napi_is_typedarray(env, argv[0], &is_typedarray) == napi_ok && is_typedarray) result = true;
    if (napi_is_dataview(env, argv[0], &is_dataview) == napi_ok && is_dataview) result = true;
  }
  napi_value out = nullptr;
  napi_get_boolean(env, result, &out);
  return out != nullptr ? out : Undefined(env);
}

napi_value UtilIsTypedArray(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  bool result = false;
  if (argc >= 1 && argv[0] != nullptr) {
    napi_is_typedarray(env, argv[0], &result);
  }
  napi_value out = nullptr;
  napi_get_boolean(env, result, &out);
  return out != nullptr ? out : Undefined(env);
}

napi_value UtilIsUint8Array(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  bool result = false;
  if (argc >= 1 && argv[0] != nullptr) {
    bool is_typedarray = false;
    if (napi_is_typedarray(env, argv[0], &is_typedarray) == napi_ok && is_typedarray) {
      napi_typedarray_type type = napi_uint8_array;
      size_t length = 0;
      void* data = nullptr;
      napi_value arraybuffer = nullptr;
      size_t offset = 0;
      if (napi_get_typedarray_info(env, argv[0], &type, &length, &data, &arraybuffer, &offset) == napi_ok) {
        result = (type == napi_uint8_array);
      }
    }
  }
  napi_value out = nullptr;
  napi_get_boolean(env, result, &out);
  return out != nullptr ? out : Undefined(env);
}

void EnsureMethodFromTypes(napi_env env, napi_value binding, const char* name, TypesAlias alias) {
  bool has = false;
  if (napi_has_named_property(env, binding, name, &has) != napi_ok || has) return;
  napi_value fn = nullptr;
  napi_create_function(env,
                       name,
                       NAPI_AUTO_LENGTH,
                       UtilTypesAliasCallback,
                       reinterpret_cast<void*>(static_cast<uintptr_t>(alias)),
                       &fn);
  if (fn != nullptr) napi_set_named_property(env, binding, name, fn);
}

void EnsureMethod(napi_env env, napi_value binding, const char* name, napi_callback cb) {
  bool has = false;
  if (napi_has_named_property(env, binding, name, &has) != napi_ok || has) return;
  napi_value fn = nullptr;
  if (napi_create_function(env, name, NAPI_AUTO_LENGTH, cb, nullptr, &fn) == napi_ok && fn != nullptr) {
    napi_set_named_property(env, binding, name, fn);
  }
}

}  // namespace

napi_value ResolveUtil(napi_env env, const ResolveOptions& /*options*/) {
  napi_value binding = GetGlobalNamed(env, "__ubi_util");
  if (binding == nullptr || IsUndefined(env, binding)) return Undefined(env);

  EnsureMethodFromTypes(env, binding, "isAnyArrayBuffer", TypesAlias::kIsAnyArrayBuffer);
  EnsureMethodFromTypes(env, binding, "isArrayBuffer", TypesAlias::kIsArrayBuffer);
  EnsureMethodFromTypes(env, binding, "isAsyncFunction", TypesAlias::kIsAsyncFunction);
  EnsureMethodFromTypes(env, binding, "isDataView", TypesAlias::kIsDataView);
  EnsureMethodFromTypes(env, binding, "isDate", TypesAlias::kIsDate);
  EnsureMethodFromTypes(env, binding, "isExternal", TypesAlias::kIsExternal);
  EnsureMethodFromTypes(env, binding, "isMap", TypesAlias::kIsMap);
  EnsureMethodFromTypes(env, binding, "isMapIterator", TypesAlias::kIsMapIterator);
  EnsureMethodFromTypes(env, binding, "isNativeError", TypesAlias::kIsNativeError);
  EnsureMethodFromTypes(env, binding, "isPromise", TypesAlias::kIsPromise);
  EnsureMethodFromTypes(env, binding, "isRegExp", TypesAlias::kIsRegExp);
  EnsureMethodFromTypes(env, binding, "isSet", TypesAlias::kIsSet);
  EnsureMethodFromTypes(env, binding, "isSetIterator", TypesAlias::kIsSetIterator);

  EnsureMethod(env, binding, "isArrayBufferView", UtilIsArrayBufferView);
  EnsureMethod(env, binding, "isTypedArray", UtilIsTypedArray);
  EnsureMethod(env, binding, "isUint8Array", UtilIsUint8Array);

  return binding;
}

}  // namespace internal_binding
