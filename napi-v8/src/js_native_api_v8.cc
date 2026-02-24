#include "internal/napi_v8_env.h"

#include <cstring>
#include <limits>
#include <memory>
#include <new>
#include <string>
#include <vector>

namespace {

struct CallbackPayload {
  napi_env env;
  napi_callback cb;
  void* data;
};

struct AccessorPayload {
  napi_env env;
  napi_callback getter_cb;
  napi_callback setter_cb;
  void* data;
};


inline bool CheckEnv(napi_env env) {
  return env != nullptr && env->isolate != nullptr;
}

inline bool CheckValue(napi_env env, napi_value value) {
  return CheckEnv(env) && value != nullptr;
}

inline napi_valuetype TypeOf(v8::Local<v8::Value> value) {
  if (value->IsUndefined()) return napi_undefined;
  if (value->IsNull()) return napi_null;
  if (value->IsBoolean()) return napi_boolean;
  if (value->IsNumber()) return napi_number;
  if (value->IsString()) return napi_string;
  if (value->IsSymbol()) return napi_symbol;
  if (value->IsFunction()) return napi_function;
  if (value->IsBigInt()) return napi_bigint;
  if (value->IsExternal()) return napi_external;
  return napi_object;
}

inline v8::PropertyAttribute ToV8PropertyAttributes(napi_property_attributes attrs,
                                                    bool include_writable) {
  int v8_attrs = v8::None;
  if ((attrs & napi_enumerable) == 0) v8_attrs |= v8::DontEnum;
  if ((attrs & napi_configurable) == 0) v8_attrs |= v8::DontDelete;
  if (include_writable && (attrs & napi_writable) == 0) v8_attrs |= v8::ReadOnly;
  return static_cast<v8::PropertyAttribute>(v8_attrs);
}

void FunctionTrampoline(const v8::FunctionCallbackInfo<v8::Value>& info) {
  auto* payload =
      static_cast<CallbackPayload*>(info.Data().As<v8::External>()->Value());
  if (payload == nullptr || payload->env == nullptr || payload->cb == nullptr) {
    return;
  }

  napi_env env = payload->env;
  auto* cbinfo = new napi_callback_info__();
  cbinfo->env = env;
  cbinfo->data = payload->data;
  cbinfo->this_arg = napi_v8_wrap_value(env, info.This());
  if (!info.NewTarget().IsEmpty()) {
    cbinfo->new_target = napi_v8_wrap_value(env, info.NewTarget());
  }
  cbinfo->args.reserve(info.Length());
  for (int i = 0; i < info.Length(); ++i) {
    cbinfo->args.push_back(napi_v8_wrap_value(env, info[i]));
  }

  napi_value ret = payload->cb(env, cbinfo);
  if (ret != nullptr) {
    info.GetReturnValue().Set(napi_v8_unwrap_value(ret));
  }

  delete cbinfo;
}

void GetterTrampoline(v8::Local<v8::Name> property,
                      const v8::PropertyCallbackInfo<v8::Value>& info) {
  (void)property;
  auto* payload =
      static_cast<AccessorPayload*>(info.Data().As<v8::External>()->Value());
  if (payload == nullptr || payload->env == nullptr || payload->getter_cb == nullptr) {
    return;
  }
  napi_env env = payload->env;
  auto* cbinfo = new napi_callback_info__();
  cbinfo->env = env;
  cbinfo->data = payload->data;
  cbinfo->this_arg = napi_v8_wrap_value(env, info.This());
  napi_value ret = payload->getter_cb(env, cbinfo);
  if (ret != nullptr) {
    info.GetReturnValue().Set(napi_v8_unwrap_value(ret));
  }
  delete cbinfo;
}

void SetterTrampoline(v8::Local<v8::Name> property,
                      v8::Local<v8::Value> value,
                      const v8::PropertyCallbackInfo<void>& info) {
  (void)property;
  auto* payload =
      static_cast<AccessorPayload*>(info.Data().As<v8::External>()->Value());
  if (payload == nullptr || payload->env == nullptr || payload->setter_cb == nullptr) {
    return;
  }
  napi_env env = payload->env;
  auto* cbinfo = new napi_callback_info__();
  cbinfo->env = env;
  cbinfo->data = payload->data;
  cbinfo->this_arg = napi_v8_wrap_value(env, info.This());
  cbinfo->args.push_back(napi_v8_wrap_value(env, value));
  payload->setter_cb(env, cbinfo);
  delete cbinfo;
}

}  // namespace

napi_value__::napi_value__(napi_env env, v8::Local<v8::Value> local)
    : env(env), value(env->isolate, local) {}

napi_value__::~napi_value__() = default;

v8::Local<v8::Value> napi_value__::local() const {
  return value.Get(env->isolate);
}

napi_ref__::napi_ref__(napi_env env,
                       v8::Local<v8::Value> value,
                       uint32_t initial_refcount)
    : env(env), value(env->isolate, value), refcount(initial_refcount) {}

napi_ref__::~napi_ref__() = default;

napi_env__::napi_env__(v8::Local<v8::Context> context, int32_t module_api_version)
    : isolate(v8::Isolate::GetCurrent()),
      context_ref(isolate, context),
      module_api_version(module_api_version) {
  v8::Local<v8::Private> wrapKey = v8::Private::ForApi(
      isolate, v8::String::NewFromUtf8Literal(isolate, "__napi_wrap"));
  wrap_private_key.Reset(isolate, wrapKey);
  v8::Local<v8::Private> wrapRefKey = v8::Private::ForApi(
      isolate, v8::String::NewFromUtf8Literal(isolate, "__napi_wrap_ref"));
  wrap_ref_private_key.Reset(isolate, wrapRefKey);
  napi_v8_clear_last_error(this);
}

napi_env__::~napi_env__() {
  if (instance_data_finalize_cb != nullptr) {
    instance_data_finalize_cb(this, instance_data, instance_data_finalize_hint);
  }
}

v8::Local<v8::Context> napi_env__::context() const {
  return context_ref.Get(isolate);
}

napi_status napi_v8_set_last_error(napi_env env,
                                   napi_status status,
                                   const char* message) {
  if (env == nullptr) return status;
  env->last_error.error_code = status;
  env->last_error.engine_error_code = 0;
  env->last_error.engine_reserved = nullptr;
  env->last_error_message = (message == nullptr) ? "" : message;
  env->last_error.error_message =
      env->last_error_message.empty() ? nullptr : env->last_error_message.c_str();
  return status;
}

napi_status napi_v8_clear_last_error(napi_env env) {
  return napi_v8_set_last_error(env, napi_ok, nullptr);
}

napi_value napi_v8_wrap_value(napi_env env, v8::Local<v8::Value> value) {
  if (!CheckEnv(env)) return nullptr;
  return new (std::nothrow) napi_value__(env, value);
}

v8::Local<v8::Value> napi_v8_unwrap_value(napi_value value) {
  return value->local();
}

extern "C" {

napi_status NAPI_CDECL napi_get_last_error_info(
    node_api_basic_env env, const napi_extended_error_info** result) {
  if (result == nullptr) return napi_invalid_arg;
  auto* napiEnv = const_cast<napi_env>(env);
  if (!CheckEnv(napiEnv)) return napi_invalid_arg;
  *result = &napiEnv->last_error;
  return napi_ok;
}

napi_status NAPI_CDECL napi_get_undefined(napi_env env, napi_value* result) {
  if (!CheckEnv(env) || result == nullptr) return napi_invalid_arg;
  *result = napi_v8_wrap_value(env, v8::Undefined(env->isolate));
  return (*result == nullptr) ? napi_generic_failure : napi_ok;
}

napi_status NAPI_CDECL napi_get_null(napi_env env, napi_value* result) {
  if (!CheckEnv(env) || result == nullptr) return napi_invalid_arg;
  *result = napi_v8_wrap_value(env, v8::Null(env->isolate));
  return (*result == nullptr) ? napi_generic_failure : napi_ok;
}

napi_status NAPI_CDECL napi_get_global(napi_env env, napi_value* result) {
  if (!CheckEnv(env) || result == nullptr) return napi_invalid_arg;
  auto context = env->context();
  *result = napi_v8_wrap_value(env, context->Global());
  return (*result == nullptr) ? napi_generic_failure : napi_ok;
}

napi_status NAPI_CDECL napi_get_boolean(napi_env env,
                                        bool value,
                                        napi_value* result) {
  if (!CheckEnv(env) || result == nullptr) return napi_invalid_arg;
  *result = napi_v8_wrap_value(env, v8::Boolean::New(env->isolate, value));
  return (*result == nullptr) ? napi_generic_failure : napi_ok;
}

napi_status NAPI_CDECL napi_create_double(napi_env env,
                                          double value,
                                          napi_value* result) {
  if (!CheckEnv(env) || result == nullptr) return napi_invalid_arg;
  *result = napi_v8_wrap_value(env, v8::Number::New(env->isolate, value));
  return (*result == nullptr) ? napi_generic_failure : napi_ok;
}

napi_status NAPI_CDECL napi_create_int32(napi_env env,
                                         int32_t value,
                                         napi_value* result) {
  if (!CheckEnv(env) || result == nullptr) return napi_invalid_arg;
  *result = napi_v8_wrap_value(env, v8::Int32::New(env->isolate, value));
  return (*result == nullptr) ? napi_generic_failure : napi_ok;
}

napi_status NAPI_CDECL napi_create_int64(napi_env env,
                                         int64_t value,
                                         napi_value* result) {
  if (!CheckEnv(env) || result == nullptr) return napi_invalid_arg;
  *result = napi_v8_wrap_value(env, v8::Number::New(env->isolate, static_cast<double>(value)));
  return (*result == nullptr) ? napi_generic_failure : napi_ok;
}

napi_status NAPI_CDECL napi_create_uint32(napi_env env,
                                          uint32_t value,
                                          napi_value* result) {
  if (!CheckEnv(env) || result == nullptr) return napi_invalid_arg;
  *result = napi_v8_wrap_value(env, v8::Integer::NewFromUnsigned(env->isolate, value));
  return (*result == nullptr) ? napi_generic_failure : napi_ok;
}

napi_status NAPI_CDECL napi_create_object(napi_env env, napi_value* result) {
  if (!CheckEnv(env) || result == nullptr) return napi_invalid_arg;
  *result = napi_v8_wrap_value(env, v8::Object::New(env->isolate));
  return (*result == nullptr) ? napi_generic_failure : napi_ok;
}

napi_status NAPI_CDECL napi_create_array(napi_env env, napi_value* result) {
  if (!CheckEnv(env) || result == nullptr) return napi_invalid_arg;
  *result = napi_v8_wrap_value(env, v8::Array::New(env->isolate));
  return (*result == nullptr) ? napi_generic_failure : napi_ok;
}

napi_status NAPI_CDECL napi_create_array_with_length(napi_env env,
                                                     size_t length,
                                                     napi_value* result) {
  if (!CheckEnv(env) || result == nullptr) return napi_invalid_arg;
  auto context = env->context();
  uint32_t length32 = static_cast<uint32_t>(
      length > static_cast<size_t>(std::numeric_limits<uint32_t>::max())
          ? std::numeric_limits<uint32_t>::max()
          : length);
  v8::Local<v8::Array> arr = v8::Array::New(env->isolate);
  if (length32 > 0) {
    if (!arr
             ->Set(context,
                   v8::String::NewFromUtf8Literal(env->isolate, "length"),
                   v8::Integer::NewFromUnsigned(env->isolate, length32))
             .FromMaybe(false)) {
      return napi_generic_failure;
    }
  }
  *result = napi_v8_wrap_value(env, arr);
  return (*result == nullptr) ? napi_generic_failure : napi_ok;
}

napi_status NAPI_CDECL napi_create_string_utf8(napi_env env,
                                               const char* str,
                                               size_t length,
                                               napi_value* result) {
  if (!CheckEnv(env) || str == nullptr || result == nullptr) return napi_invalid_arg;
  auto context = env->context();
  const int v8Length =
      (length == NAPI_AUTO_LENGTH) ? -1 : static_cast<int>(length);
  v8::MaybeLocal<v8::String> maybe =
      v8::String::NewFromUtf8(env->isolate, str, v8::NewStringType::kNormal, v8Length);
  v8::Local<v8::String> out;
  if (!maybe.ToLocal(&out)) return napi_v8_set_last_error(env, napi_generic_failure, "Cannot create string");
  *result = napi_v8_wrap_value(env, out);
  return (*result == nullptr) ? napi_generic_failure : napi_ok;
}

napi_status NAPI_CDECL napi_typeof(napi_env env,
                                   napi_value value,
                                   napi_valuetype* result) {
  if (!CheckValue(env, value) || result == nullptr) return napi_invalid_arg;
  *result = TypeOf(napi_v8_unwrap_value(value));
  return napi_ok;
}

napi_status NAPI_CDECL napi_get_value_double(napi_env env,
                                             napi_value value,
                                             double* result) {
  if (!CheckValue(env, value) || result == nullptr) return napi_invalid_arg;
  v8::Local<v8::Value> local = napi_v8_unwrap_value(value);
  if (!local->IsNumber()) return napi_number_expected;
  *result = local.As<v8::Number>()->Value();
  return napi_ok;
}

napi_status NAPI_CDECL napi_get_value_uint32(napi_env env,
                                             napi_value value,
                                             uint32_t* result) {
  if (!CheckValue(env, value) || result == nullptr) return napi_invalid_arg;
  v8::Local<v8::Value> local = napi_v8_unwrap_value(value);
  if (!local->IsNumber()) return napi_number_expected;
  *result = local->Uint32Value(env->context()).FromMaybe(0);
  return napi_ok;
}

napi_status NAPI_CDECL napi_get_value_int32(napi_env env,
                                            napi_value value,
                                            int32_t* result) {
  if (!CheckValue(env, value) || result == nullptr) return napi_invalid_arg;
  v8::Local<v8::Value> local = napi_v8_unwrap_value(value);
  if (!local->IsNumber()) return napi_number_expected;
  *result = local->Int32Value(env->context()).FromMaybe(0);
  return napi_ok;
}

napi_status NAPI_CDECL napi_get_value_int64(napi_env env,
                                            napi_value value,
                                            int64_t* result) {
  if (!CheckValue(env, value) || result == nullptr) return napi_invalid_arg;
  v8::Local<v8::Value> local = napi_v8_unwrap_value(value);
  if (!local->IsNumber()) return napi_number_expected;
  *result = local->IntegerValue(env->context()).FromMaybe(0);
  return napi_ok;
}

napi_status NAPI_CDECL napi_is_array(napi_env env, napi_value value, bool* result) {
  if (!CheckValue(env, value) || result == nullptr) return napi_invalid_arg;
  *result = napi_v8_unwrap_value(value)->IsArray();
  return napi_ok;
}

napi_status NAPI_CDECL napi_get_array_length(napi_env env,
                                             napi_value value,
                                             uint32_t* result) {
  if (!CheckValue(env, value) || result == nullptr) return napi_invalid_arg;
  v8::Local<v8::Value> local = napi_v8_unwrap_value(value);
  if (!local->IsArray()) return napi_array_expected;
  *result = local.As<v8::Array>()->Length();
  return napi_ok;
}

napi_status NAPI_CDECL napi_get_element(napi_env env,
                                        napi_value object,
                                        uint32_t index,
                                        napi_value* result) {
  if (!CheckValue(env, object) || result == nullptr) return napi_invalid_arg;
  v8::Local<v8::Value> local = napi_v8_unwrap_value(object);
  if (!local->IsObject()) return napi_object_expected;
  v8::Local<v8::Value> out;
  if (!local.As<v8::Object>()->Get(env->context(), index).ToLocal(&out)) {
    return napi_generic_failure;
  }
  *result = napi_v8_wrap_value(env, out);
  return (*result == nullptr) ? napi_generic_failure : napi_ok;
}

napi_status NAPI_CDECL napi_set_element(napi_env env,
                                        napi_value object,
                                        uint32_t index,
                                        napi_value value) {
  if (!CheckValue(env, object) || value == nullptr) return napi_invalid_arg;
  v8::Local<v8::Value> local = napi_v8_unwrap_value(object);
  if (!local->IsObject()) return napi_object_expected;
  if (!local.As<v8::Object>()
           ->Set(env->context(), index, napi_v8_unwrap_value(value))
           .FromMaybe(false)) {
    return napi_generic_failure;
  }
  return napi_ok;
}

napi_status NAPI_CDECL napi_has_element(napi_env env,
                                        napi_value object,
                                        uint32_t index,
                                        bool* result) {
  if (!CheckValue(env, object) || result == nullptr) return napi_invalid_arg;
  v8::Local<v8::Value> local = napi_v8_unwrap_value(object);
  if (!local->IsObject()) return napi_object_expected;
  *result = local.As<v8::Object>()->Has(env->context(), index).FromMaybe(false);
  return napi_ok;
}

napi_status NAPI_CDECL napi_delete_element(napi_env env,
                                           napi_value object,
                                           uint32_t index,
                                           bool* result) {
  if (!CheckValue(env, object) || result == nullptr) return napi_invalid_arg;
  v8::Local<v8::Value> local = napi_v8_unwrap_value(object);
  if (!local->IsObject()) return napi_object_expected;
  *result = local.As<v8::Object>()->Delete(env->context(), index).FromMaybe(false);
  return napi_ok;
}

napi_status NAPI_CDECL napi_get_cb_info(napi_env env,
                                        napi_callback_info cbinfo,
                                        size_t* argc,
                                        napi_value* argv,
                                        napi_value* this_arg,
                                        void** data) {
  if (!CheckEnv(env) || cbinfo == nullptr) return napi_invalid_arg;
  size_t provided = (argc == nullptr) ? 0 : *argc;
  if (argc != nullptr) {
    *argc = cbinfo->args.size();
  }
  if (argv != nullptr && provided > 0) {
    const size_t n = (provided < cbinfo->args.size()) ? provided : cbinfo->args.size();
    for (size_t i = 0; i < n; ++i) argv[i] = cbinfo->args[i];
    for (size_t i = n; i < provided; ++i) {
      argv[i] = napi_v8_wrap_value(env, v8::Undefined(env->isolate));
    }
  }
  if (this_arg != nullptr) *this_arg = cbinfo->this_arg;
  if (data != nullptr) *data = cbinfo->data;
  return napi_ok;
}

napi_status NAPI_CDECL napi_get_new_target(
    napi_env env, napi_callback_info cbinfo, napi_value* result) {
  if (!CheckEnv(env) || cbinfo == nullptr || result == nullptr) {
    return napi_invalid_arg;
  }
  *result = cbinfo->new_target;
  return napi_ok;
}

napi_status NAPI_CDECL napi_create_function(napi_env env,
                                            const char* utf8name,
                                            size_t length,
                                            napi_callback cb,
                                            void* data,
                                            napi_value* result) {
  if (!CheckEnv(env) || cb == nullptr || result == nullptr) return napi_invalid_arg;
  auto* payload = new (std::nothrow) CallbackPayload{env, cb, data};
  if (payload == nullptr) return napi_generic_failure;

  v8::Local<v8::External> payloadValue = v8::External::New(env->isolate, payload);
  v8::Local<v8::Context> context = env->context();

  v8::Local<v8::String> name;
  if (utf8name != nullptr) {
    const int v8Length =
        (length == NAPI_AUTO_LENGTH) ? -1 : static_cast<int>(length);
    v8::MaybeLocal<v8::String> maybeName =
        v8::String::NewFromUtf8(env->isolate, utf8name, v8::NewStringType::kNormal, v8Length);
    if (!maybeName.ToLocal(&name)) return napi_generic_failure;
  }

  v8::MaybeLocal<v8::Function> maybeFn = v8::Function::New(
      context, FunctionTrampoline, payloadValue);
  v8::Local<v8::Function> fn;
  if (!maybeFn.ToLocal(&fn)) return napi_generic_failure;
  if (!name.IsEmpty()) fn->SetName(name);

  *result = napi_v8_wrap_value(env, fn);
  return (*result == nullptr) ? napi_generic_failure : napi_ok;
}

napi_status NAPI_CDECL napi_define_class(napi_env env,
                                         const char* utf8name,
                                         size_t length,
                                         napi_callback constructor,
                                         void* data,
                                         size_t property_count,
                                         const napi_property_descriptor* properties,
                                         napi_value* result) {
  if (!CheckEnv(env)) {
    return napi_invalid_arg;
  }
  if (utf8name == nullptr || constructor == nullptr || result == nullptr) {
    return napi_v8_set_last_error(env, napi_invalid_arg, "Invalid argument");
  }
  if (property_count > 0 && properties == nullptr) {
    return napi_v8_set_last_error(env, napi_invalid_arg, "Invalid argument");
  }

  napi_value ctorValue = nullptr;
  napi_status status =
      napi_create_function(env, utf8name, length, constructor, data, &ctorValue);
  if (status != napi_ok) return status;

  v8::Local<v8::Context> context = env->context();
  v8::Local<v8::Function> ctor = napi_v8_unwrap_value(ctorValue).As<v8::Function>();
  v8::Local<v8::Object> proto = ctor->Get(context, v8::String::NewFromUtf8Literal(env->isolate, "prototype"))
                                     .ToLocalChecked()
                                     .As<v8::Object>();

  for (size_t i = 0; i < property_count; ++i) {
    const napi_property_descriptor& desc = properties[i];
    if (desc.utf8name == nullptr) return napi_name_expected;
    v8::Local<v8::String> key;
    if (!v8::String::NewFromUtf8(env->isolate, desc.utf8name, v8::NewStringType::kNormal)
             .ToLocal(&key)) {
      return napi_generic_failure;
    }
    v8::Local<v8::Object> target =
        (desc.attributes & napi_static) ? ctor.As<v8::Object>() : proto;

    if (desc.method != nullptr) {
      napi_value fnValue = nullptr;
      status = napi_create_function(
          env, desc.utf8name, NAPI_AUTO_LENGTH, desc.method, desc.data, &fnValue);
      if (status != napi_ok) return status;
      if (!target->DefineOwnProperty(
               context,
               key,
               napi_v8_unwrap_value(fnValue),
               ToV8PropertyAttributes(desc.attributes, true))
               .FromMaybe(false)) {
        return napi_generic_failure;
      }
      continue;
    }

    if (desc.getter != nullptr || desc.setter != nullptr) {
      v8::Local<v8::Function> getter_fn;
      v8::Local<v8::Function> setter_fn;
      if (desc.getter != nullptr) {
        napi_value getter_value = nullptr;
        status = napi_create_function(
            env, desc.utf8name, NAPI_AUTO_LENGTH, desc.getter, desc.data, &getter_value);
        if (status != napi_ok) return status;
        getter_fn = napi_v8_unwrap_value(getter_value).As<v8::Function>();
      }
      if (desc.setter != nullptr) {
        napi_value setter_value = nullptr;
        status = napi_create_function(
            env, desc.utf8name, NAPI_AUTO_LENGTH, desc.setter, desc.data, &setter_value);
        if (status != napi_ok) return status;
        setter_fn = napi_v8_unwrap_value(setter_value).As<v8::Function>();
      }
      target->SetAccessorProperty(
          key,
          getter_fn,
          setter_fn,
          ToV8PropertyAttributes(desc.attributes, false));
      continue;
    }

    if (desc.value != nullptr) {
      if (!target->DefineOwnProperty(
               context,
               key,
               napi_v8_unwrap_value(desc.value),
               ToV8PropertyAttributes(desc.attributes, true))
               .FromMaybe(false)) {
        return napi_generic_failure;
      }
      continue;
    }
  }

  *result = ctorValue;
  return napi_v8_clear_last_error(env);
}

napi_status NAPI_CDECL napi_new_instance(napi_env env,
                                         napi_value constructor,
                                         size_t argc,
                                         const napi_value* argv,
                                         napi_value* result) {
  if (!CheckValue(env, constructor) || result == nullptr) return napi_invalid_arg;
  v8::Local<v8::Value> ctorValue = napi_v8_unwrap_value(constructor);
  if (!ctorValue->IsFunction()) return napi_function_expected;
  v8::Local<v8::Function> ctor = ctorValue.As<v8::Function>();
  std::vector<v8::Local<v8::Value>> args;
  args.reserve(argc);
  for (size_t i = 0; i < argc; ++i) args.push_back(napi_v8_unwrap_value(argv[i]));
  v8::Local<v8::Value> out;
  if (!ctor->NewInstance(env->context(), static_cast<int>(argc), args.data())
           .ToLocal(&out)) {
    return napi_generic_failure;
  }
  *result = napi_v8_wrap_value(env, out);
  return (*result == nullptr) ? napi_generic_failure : napi_ok;
}

napi_status NAPI_CDECL napi_call_function(napi_env env,
                                          napi_value recv,
                                          napi_value func,
                                          size_t argc,
                                          const napi_value* argv,
                                          napi_value* result) {
  if (!CheckValue(env, recv) || !CheckValue(env, func)) return napi_invalid_arg;
  auto context = env->context();
  v8::Local<v8::Function> fn;
  if (!napi_v8_unwrap_value(func)->IsFunction()) return napi_function_expected;
  fn = napi_v8_unwrap_value(func).As<v8::Function>();

  std::vector<v8::Local<v8::Value>> args;
  args.reserve(argc);
  for (size_t i = 0; i < argc; ++i) {
    args.push_back(napi_v8_unwrap_value(argv[i]));
  }

  v8::TryCatch tryCatch(env->isolate);
  v8::MaybeLocal<v8::Value> maybe = fn->Call(
      context, napi_v8_unwrap_value(recv), argc, args.data());
  if (tryCatch.HasCaught()) {
    env->last_exception.Reset(env->isolate, tryCatch.Exception());
    return napi_v8_set_last_error(env, napi_pending_exception, "Function call threw");
  }
  if (result != nullptr) {
    v8::Local<v8::Value> out;
    if (!maybe.ToLocal(&out)) return napi_generic_failure;
    *result = napi_v8_wrap_value(env, out);
  }
  return napi_ok;
}

napi_status NAPI_CDECL napi_define_properties(
    napi_env env,
    napi_value object,
    size_t property_count,
    const napi_property_descriptor* properties) {
  if (!CheckValue(env, object) || properties == nullptr) return napi_invalid_arg;
  auto context = env->context();
  v8::Local<v8::Value> targetValue = napi_v8_unwrap_value(object);
  if (!targetValue->IsObject()) return napi_object_expected;
  v8::Local<v8::Object> target = targetValue.As<v8::Object>();

  for (size_t i = 0; i < property_count; ++i) {
    const napi_property_descriptor& desc = properties[i];
    if (desc.utf8name == nullptr) return napi_name_expected;
    v8::Local<v8::String> key;
    if (!v8::String::NewFromUtf8(
             env->isolate, desc.utf8name, v8::NewStringType::kNormal)
             .ToLocal(&key)) {
      return napi_generic_failure;
    }

    if (desc.method != nullptr) {
      napi_value fnValue = nullptr;
      napi_status status = napi_create_function(
          env, desc.utf8name, NAPI_AUTO_LENGTH, desc.method, desc.data, &fnValue);
      if (status != napi_ok) return status;
      if (!target->DefineOwnProperty(
               context,
               key,
               napi_v8_unwrap_value(fnValue),
               ToV8PropertyAttributes(desc.attributes, true))
               .FromMaybe(false)) {
        return napi_generic_failure;
      }
      continue;
    }

    if (desc.getter != nullptr || desc.setter != nullptr) {
      napi_status status = napi_ok;
      v8::Local<v8::Function> getter_fn;
      v8::Local<v8::Function> setter_fn;
      if (desc.getter != nullptr) {
        napi_value getter_value = nullptr;
        status = napi_create_function(
            env, desc.utf8name, NAPI_AUTO_LENGTH, desc.getter, desc.data, &getter_value);
        if (status != napi_ok) return status;
        getter_fn = napi_v8_unwrap_value(getter_value).As<v8::Function>();
      }
      if (desc.setter != nullptr) {
        napi_value setter_value = nullptr;
        status = napi_create_function(
            env, desc.utf8name, NAPI_AUTO_LENGTH, desc.setter, desc.data, &setter_value);
        if (status != napi_ok) return status;
        setter_fn = napi_v8_unwrap_value(setter_value).As<v8::Function>();
      }
      target->SetAccessorProperty(
          key,
          getter_fn,
          setter_fn,
          ToV8PropertyAttributes(desc.attributes, false));
      continue;
    }

    if (desc.value != nullptr) {
      if (!target->DefineOwnProperty(
               context,
               key,
               napi_v8_unwrap_value(desc.value),
               ToV8PropertyAttributes(desc.attributes, true))
               .FromMaybe(false)) {
        return napi_generic_failure;
      }
    }
  }

  return napi_ok;
}

napi_status NAPI_CDECL napi_set_named_property(napi_env env,
                                               napi_value object,
                                               const char* utf8name,
                                               napi_value value) {
  if (!CheckValue(env, object) || utf8name == nullptr || value == nullptr) {
    return napi_invalid_arg;
  }
  auto context = env->context();
  v8::Local<v8::Value> targetValue = napi_v8_unwrap_value(object);
  if (!targetValue->IsObject()) return napi_object_expected;
  v8::Local<v8::String> key;
  if (!v8::String::NewFromUtf8(
           env->isolate, utf8name, v8::NewStringType::kNormal)
           .ToLocal(&key)) {
    return napi_generic_failure;
  }
  if (!targetValue.As<v8::Object>()
           ->Set(context, key, napi_v8_unwrap_value(value))
           .FromMaybe(false)) {
    return napi_generic_failure;
  }
  return napi_ok;
}

napi_status NAPI_CDECL napi_get_named_property(napi_env env,
                                               napi_value object,
                                               const char* utf8name,
                                               napi_value* result) {
  if (!CheckValue(env, object) || utf8name == nullptr || result == nullptr) {
    return napi_invalid_arg;
  }
  auto context = env->context();
  v8::Local<v8::Value> targetValue = napi_v8_unwrap_value(object);
  if (!targetValue->IsObject()) return napi_object_expected;
  v8::Local<v8::String> key;
  if (!v8::String::NewFromUtf8(
           env->isolate, utf8name, v8::NewStringType::kNormal)
           .ToLocal(&key)) {
    return napi_generic_failure;
  }
  v8::Local<v8::Value> prop;
  if (!targetValue.As<v8::Object>()->Get(context, key).ToLocal(&prop)) {
    return napi_generic_failure;
  }
  *result = napi_v8_wrap_value(env, prop);
  return (*result == nullptr) ? napi_generic_failure : napi_ok;
}

napi_status NAPI_CDECL napi_get_value_bool(napi_env env,
                                           napi_value value,
                                           bool* result) {
  if (!CheckValue(env, value) || result == nullptr) return napi_invalid_arg;
  v8::Local<v8::Value> local = napi_v8_unwrap_value(value);
  if (!local->IsBoolean()) return napi_boolean_expected;
  *result = local.As<v8::Boolean>()->Value();
  return napi_ok;
}

napi_status NAPI_CDECL napi_get_value_string_utf8(
    napi_env env, napi_value value, char* buf, size_t bufsize, size_t* result) {
  if (!CheckValue(env, value)) return napi_invalid_arg;
  v8::Local<v8::Value> local = napi_v8_unwrap_value(value);
  if (!local->IsString()) return napi_string_expected;

  v8::String::Utf8Value utf8(env->isolate, local);
  if (*utf8 == nullptr) return napi_generic_failure;
  size_t len = std::strlen(*utf8);

  if (result != nullptr) {
    *result = len;
  }
  if (buf != nullptr && bufsize > 0) {
    size_t to_copy = (len < (bufsize - 1)) ? len : (bufsize - 1);
    std::memcpy(buf, *utf8, to_copy);
    buf[to_copy] = '\0';
  }
  return napi_ok;
}

napi_status NAPI_CDECL napi_strict_equals(napi_env env,
                                          napi_value lhs,
                                          napi_value rhs,
                                          bool* result) {
  if (!CheckValue(env, lhs) || !CheckValue(env, rhs) || result == nullptr) {
    return napi_invalid_arg;
  }
  *result = napi_v8_unwrap_value(lhs)->StrictEquals(napi_v8_unwrap_value(rhs));
  return napi_ok;
}

napi_status NAPI_CDECL napi_create_reference(napi_env env,
                                             napi_value value,
                                             uint32_t initial_refcount,
                                             napi_ref* result) {
  if (!CheckValue(env, value) || result == nullptr) return napi_invalid_arg;
  *result = new (std::nothrow)
      napi_ref__(env, napi_v8_unwrap_value(value), initial_refcount);
  return (*result == nullptr) ? napi_generic_failure : napi_ok;
}

napi_status NAPI_CDECL napi_delete_reference(node_api_basic_env env, napi_ref ref) {
  (void)env;
  if (ref == nullptr) return napi_invalid_arg;
  delete ref;
  return napi_ok;
}

napi_status NAPI_CDECL napi_reference_ref(napi_env env,
                                          napi_ref ref,
                                          uint32_t* result) {
  if (!CheckEnv(env) || ref == nullptr) return napi_invalid_arg;
  ref->refcount++;
  if (result != nullptr) *result = ref->refcount;
  return napi_ok;
}

napi_status NAPI_CDECL napi_reference_unref(napi_env env,
                                            napi_ref ref,
                                            uint32_t* result) {
  if (!CheckEnv(env) || ref == nullptr) return napi_invalid_arg;
  if (ref->refcount > 0) ref->refcount--;
  if (result != nullptr) *result = ref->refcount;
  return napi_ok;
}

napi_status NAPI_CDECL napi_get_reference_value(napi_env env,
                                                napi_ref ref,
                                                napi_value* result) {
  if (!CheckEnv(env) || ref == nullptr || result == nullptr) return napi_invalid_arg;
  *result = napi_v8_wrap_value(env, ref->value.Get(env->isolate));
  return (*result == nullptr) ? napi_generic_failure : napi_ok;
}

napi_status NAPI_CDECL napi_wrap(napi_env env,
                                 napi_value js_object,
                                 void* native_object,
                                 node_api_basic_finalize finalize_cb,
                                 void* finalize_hint,
                                 napi_ref* result) {
  (void)finalize_hint;
  if (!CheckValue(env, js_object)) return napi_invalid_arg;
  v8::Local<v8::Value> value = napi_v8_unwrap_value(js_object);
  if (!value->IsObject()) return napi_object_expected;
  v8::Local<v8::Object> object = value.As<v8::Object>();
  v8::Local<v8::Private> wrapKey = env->wrap_private_key.Get(env->isolate);
  if (!object->SetPrivate(env->context(), wrapKey, v8::External::New(env->isolate, native_object))
           .FromMaybe(false)) {
    return napi_generic_failure;
  }
  if (result != nullptr) {
    napi_status s = napi_create_reference(env, js_object, 0, result);
    if (s != napi_ok) return s;
    v8::Local<v8::Private> wrapRefKey = env->wrap_ref_private_key.Get(env->isolate);
    object
        ->SetPrivate(env->context(), wrapRefKey, v8::External::New(env->isolate, *result))
        .FromMaybe(false);
  }
  // Finalizers are not yet tied to GC in this initial implementation.
  (void)finalize_cb;
  return napi_ok;
}

napi_status NAPI_CDECL napi_unwrap(napi_env env, napi_value js_object, void** result) {
  if (!CheckValue(env, js_object) || result == nullptr) return napi_invalid_arg;
  v8::Local<v8::Value> value = napi_v8_unwrap_value(js_object);
  if (!value->IsObject()) return napi_object_expected;
  v8::Local<v8::Object> object = value.As<v8::Object>();
  v8::Local<v8::Private> wrapKey = env->wrap_private_key.Get(env->isolate);
  v8::Local<v8::Value> wrapped;
  if (!object->GetPrivate(env->context(), wrapKey).ToLocal(&wrapped) ||
      !wrapped->IsExternal()) {
    return napi_invalid_arg;
  }
  *result = wrapped.As<v8::External>()->Value();
  return napi_ok;
}

napi_status NAPI_CDECL napi_remove_wrap(napi_env env, napi_value js_object, void** result) {
  if (!CheckValue(env, js_object)) return napi_invalid_arg;
  void* out = nullptr;
  napi_status status = napi_unwrap(env, js_object, &out);
  if (status != napi_ok) return status;
  v8::Local<v8::Object> object = napi_v8_unwrap_value(js_object).As<v8::Object>();
  v8::Local<v8::Private> wrapKey = env->wrap_private_key.Get(env->isolate);
  object->DeletePrivate(env->context(), wrapKey).FromMaybe(false);
  if (result != nullptr) *result = out;
  return napi_ok;
}

napi_status NAPI_CDECL napi_throw_error(napi_env env,
                                        const char* code,
                                        const char* msg) {
  if (!CheckEnv(env)) return napi_invalid_arg;
  v8::Local<v8::String> message;
  if (!v8::String::NewFromUtf8(
           env->isolate, (msg == nullptr) ? "N-API error" : msg, v8::NewStringType::kNormal)
           .ToLocal(&message)) {
    return napi_generic_failure;
  }
  v8::Local<v8::Object> err_obj = v8::Exception::Error(message).As<v8::Object>();
  if (code != nullptr) {
    v8::Local<v8::String> code_key = v8::String::NewFromUtf8Literal(env->isolate, "code");
    v8::Local<v8::String> code_val;
    if (v8::String::NewFromUtf8(env->isolate, code, v8::NewStringType::kNormal).ToLocal(&code_val)) {
      err_obj->Set(env->context(), code_key, code_val).FromMaybe(false);
    }
  }
  env->isolate->ThrowException(err_obj);
  env->last_exception.Reset(env->isolate, err_obj);
  return napi_pending_exception;
}

napi_status NAPI_CDECL napi_throw(napi_env env, napi_value error) {
  if (!CheckValue(env, error)) return napi_invalid_arg;
  v8::Local<v8::Value> ex = napi_v8_unwrap_value(error);
  env->isolate->ThrowException(ex);
  env->last_exception.Reset(env->isolate, ex);
  return napi_pending_exception;
}

napi_status NAPI_CDECL napi_is_error(napi_env env, napi_value value, bool* result) {
  if (!CheckValue(env, value) || result == nullptr) return napi_invalid_arg;
  v8::Local<v8::Value> v = napi_v8_unwrap_value(value);
  *result = v->IsNativeError();
  return napi_ok;
}

static napi_status CreateErrorCommon(napi_env env,
                                     v8::Local<v8::Value> (*factory)(v8::Local<v8::String>),
                                     napi_value code,
                                     napi_value msg,
                                     napi_value* result) {
  if (!CheckEnv(env) || msg == nullptr || result == nullptr) return napi_invalid_arg;
  v8::Local<v8::Value> msg_val = napi_v8_unwrap_value(msg);
  if (!msg_val->IsString()) return napi_string_expected;
  v8::Local<v8::String> message = msg_val.As<v8::String>();
  v8::Local<v8::Value> created = factory(message);
  if (!created->IsObject()) return napi_generic_failure;
  v8::Local<v8::Object> err_obj = created.As<v8::Object>();
  if (code != nullptr) {
    v8::Local<v8::String> code_key = v8::String::NewFromUtf8Literal(env->isolate, "code");
    err_obj->Set(env->context(), code_key, napi_v8_unwrap_value(code)).FromMaybe(false);
  }
  *result = napi_v8_wrap_value(env, err_obj);
  return (*result == nullptr) ? napi_generic_failure : napi_ok;
}

napi_status NAPI_CDECL napi_create_error(napi_env env,
                                         napi_value code,
                                         napi_value msg,
                                         napi_value* result) {
  return CreateErrorCommon(
      env,
      [](v8::Local<v8::String> message) { return v8::Exception::Error(message); },
      code,
      msg,
      result);
}

napi_status NAPI_CDECL napi_create_type_error(napi_env env,
                                              napi_value code,
                                              napi_value msg,
                                              napi_value* result) {
  return CreateErrorCommon(
      env,
      [](v8::Local<v8::String> message) { return v8::Exception::TypeError(message); },
      code,
      msg,
      result);
}

napi_status NAPI_CDECL napi_create_range_error(napi_env env,
                                               napi_value code,
                                               napi_value msg,
                                               napi_value* result) {
  return CreateErrorCommon(
      env,
      [](v8::Local<v8::String> message) { return v8::Exception::RangeError(message); },
      code,
      msg,
      result);
}

napi_status NAPI_CDECL node_api_create_syntax_error(napi_env env,
                                                    napi_value code,
                                                    napi_value msg,
                                                    napi_value* result) {
  return CreateErrorCommon(
      env,
      [](v8::Local<v8::String> message) { return v8::Exception::SyntaxError(message); },
      code,
      msg,
      result);
}

napi_status NAPI_CDECL napi_throw_type_error(napi_env env,
                                             const char* code,
                                             const char* msg) {
  if (!CheckEnv(env)) return napi_invalid_arg;
  v8::Local<v8::String> message;
  if (!v8::String::NewFromUtf8(env->isolate,
                               (msg == nullptr) ? "Type error" : msg,
                               v8::NewStringType::kNormal)
           .ToLocal(&message)) {
    return napi_generic_failure;
  }
  v8::Local<v8::Object> err = v8::Exception::TypeError(message).As<v8::Object>();
  if (code != nullptr) {
    v8::Local<v8::String> code_key = v8::String::NewFromUtf8Literal(env->isolate, "code");
    v8::Local<v8::String> code_val;
    if (v8::String::NewFromUtf8(env->isolate, code, v8::NewStringType::kNormal).ToLocal(&code_val)) {
      err->Set(env->context(), code_key, code_val).FromMaybe(false);
    }
  }
  env->isolate->ThrowException(err);
  env->last_exception.Reset(env->isolate, err);
  return napi_pending_exception;
}

napi_status NAPI_CDECL napi_throw_range_error(napi_env env,
                                              const char* code,
                                              const char* msg) {
  if (!CheckEnv(env)) return napi_invalid_arg;
  v8::Local<v8::String> message;
  if (!v8::String::NewFromUtf8(env->isolate,
                               (msg == nullptr) ? "Range error" : msg,
                               v8::NewStringType::kNormal)
           .ToLocal(&message)) {
    return napi_generic_failure;
  }
  v8::Local<v8::Object> err = v8::Exception::RangeError(message).As<v8::Object>();
  if (code != nullptr) {
    v8::Local<v8::String> code_key = v8::String::NewFromUtf8Literal(env->isolate, "code");
    v8::Local<v8::String> code_val;
    if (v8::String::NewFromUtf8(env->isolate, code, v8::NewStringType::kNormal).ToLocal(&code_val)) {
      err->Set(env->context(), code_key, code_val).FromMaybe(false);
    }
  }
  env->isolate->ThrowException(err);
  env->last_exception.Reset(env->isolate, err);
  return napi_pending_exception;
}

napi_status NAPI_CDECL node_api_throw_syntax_error(napi_env env,
                                                   const char* code,
                                                   const char* msg) {
  if (!CheckEnv(env)) return napi_invalid_arg;
  v8::Local<v8::String> message;
  if (!v8::String::NewFromUtf8(env->isolate,
                               (msg == nullptr) ? "Syntax error" : msg,
                               v8::NewStringType::kNormal)
           .ToLocal(&message)) {
    return napi_generic_failure;
  }
  v8::Local<v8::Object> err = v8::Exception::SyntaxError(message).As<v8::Object>();
  if (code != nullptr) {
    v8::Local<v8::String> code_key = v8::String::NewFromUtf8Literal(env->isolate, "code");
    v8::Local<v8::String> code_val;
    if (v8::String::NewFromUtf8(env->isolate, code, v8::NewStringType::kNormal).ToLocal(&code_val)) {
      err->Set(env->context(), code_key, code_val).FromMaybe(false);
    }
  }
  env->isolate->ThrowException(err);
  env->last_exception.Reset(env->isolate, err);
  return napi_pending_exception;
}

napi_status NAPI_CDECL napi_is_exception_pending(napi_env env, bool* result) {
  if (!CheckEnv(env) || result == nullptr) return napi_invalid_arg;
  *result = !env->last_exception.IsEmpty();
  return napi_ok;
}

napi_status NAPI_CDECL napi_get_and_clear_last_exception(napi_env env,
                                                         napi_value* result) {
  if (!CheckEnv(env) || result == nullptr) return napi_invalid_arg;
  if (env->last_exception.IsEmpty()) return napi_generic_failure;
  v8::Local<v8::Value> ex = env->last_exception.Get(env->isolate);
  env->last_exception.Reset();
  *result = napi_v8_wrap_value(env, ex);
  return (*result == nullptr) ? napi_generic_failure : napi_ok;
}

napi_status NAPI_CDECL napi_set_instance_data(node_api_basic_env basic_env,
                                              void* data,
                                              napi_finalize finalize_cb,
                                              void* finalize_hint) {
  napi_env env = const_cast<napi_env>(basic_env);
  if (!CheckEnv(env)) return napi_invalid_arg;
  env->instance_data = data;
  env->instance_data_finalize_cb = finalize_cb;
  env->instance_data_finalize_hint = finalize_hint;
  return napi_ok;
}

napi_status NAPI_CDECL napi_get_instance_data(node_api_basic_env basic_env,
                                              void** data) {
  napi_env env = const_cast<napi_env>(basic_env);
  if (!CheckEnv(env) || data == nullptr) return napi_invalid_arg;
  *data = env->instance_data;
  return napi_ok;
}

napi_status NAPI_CDECL napi_get_version(node_api_basic_env env, uint32_t* result) {
  if (result == nullptr) return napi_invalid_arg;
  auto* napiEnv = const_cast<napi_env>(env);
  if (!CheckEnv(napiEnv)) return napi_invalid_arg;
  *result = napiEnv->module_api_version;
  return napi_ok;
}

}  // extern "C"
