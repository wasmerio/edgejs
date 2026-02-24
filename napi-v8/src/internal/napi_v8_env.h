#ifndef NAPI_V8_ENV_H_
#define NAPI_V8_ENV_H_

#include <memory>
#include <string>
#include <vector>

#include <v8.h>

#include "js_native_api.h"

struct napi_value__ {
  explicit napi_value__(napi_env env, v8::Local<v8::Value> local);
  ~napi_value__();

  v8::Local<v8::Value> local() const;

  napi_env env;
  v8::Global<v8::Value> value;
};

struct napi_callback_info__ {
  napi_env env = nullptr;
  void* data = nullptr;
  napi_value this_arg = nullptr;
  napi_value new_target = nullptr;
  std::vector<napi_value> args;
};

struct napi_ref__ {
  napi_ref__(napi_env env, v8::Local<v8::Value> value, uint32_t initial_refcount);
  ~napi_ref__();

  napi_env env = nullptr;
  v8::Global<v8::Value> value;
  uint32_t refcount = 0;
};

struct napi_env__ {
  struct TypeTagEntry {
    v8::Global<v8::Value> value;
    napi_type_tag tag{};
  };

  explicit napi_env__(v8::Local<v8::Context> context, int32_t module_api_version);
  ~napi_env__();

  v8::Local<v8::Context> context() const;

  v8::Isolate* isolate = nullptr;
  v8::Global<v8::Context> context_ref;
  napi_extended_error_info last_error{};
  std::string last_error_message;
  v8::Global<v8::Value> last_exception;
  v8::Global<v8::Private> wrap_private_key;
  v8::Global<v8::Private> wrap_ref_private_key;
  v8::Global<v8::Private> wrap_finalizer_private_key;
  int32_t module_api_version = 8;
  void* instance_data = nullptr;
  napi_finalize instance_data_finalize_cb = nullptr;
  void* instance_data_finalize_hint = nullptr;
  std::vector<void*> threadsafe_functions;
  std::vector<void*> wrap_finalizers;
  std::vector<TypeTagEntry> type_tag_entries;
};

napi_status napi_v8_set_last_error(napi_env env,
                                   napi_status status,
                                   const char* message);

napi_status napi_v8_clear_last_error(napi_env env);

napi_value napi_v8_wrap_value(napi_env env, v8::Local<v8::Value> value);
v8::Local<v8::Value> napi_v8_unwrap_value(napi_value value);

#endif  // NAPI_V8_ENV_H_
