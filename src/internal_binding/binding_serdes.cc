#include "internal_binding/binding_initializers.h"

#include "edge_buffer.h"
#include "unofficial_napi.h"
#include "internal_binding/helpers.h"

namespace internal_binding {

namespace {

// Serializer#releaseBuffer: the provider returns the bytes as a plain
// Uint8Array; v8.serialize() promises a Buffer, which this makes it in place.
napi_value SerializerReleaseBuffer(napi_env env, napi_callback_info info) {
  size_t argc = 0;
  napi_value this_arg = nullptr;
  void* data = nullptr;
  if (napi_get_cb_info(env, info, &argc, nullptr, &this_arg, &data) != napi_ok || data == nullptr) {
    return nullptr;
  }
  napi_value release = nullptr;
  if (napi_get_reference_value(env, static_cast<napi_ref>(data), &release) != napi_ok ||
      release == nullptr) {
    return nullptr;
  }
  napi_value bytes = nullptr;
  if (napi_call_function(env, this_arg, release, 0, nullptr, &bytes) != napi_ok) return nullptr;
  return EdgeMakeBuffer(env, bytes);
}

void WrapReleaseBuffer(napi_env env, napi_value binding) {
  napi_value serializer = nullptr;
  napi_value prototype = nullptr;
  napi_value release = nullptr;
  napi_valuetype type = napi_undefined;
  if (napi_get_named_property(env, binding, "Serializer", &serializer) != napi_ok ||
      napi_get_named_property(env, serializer, "prototype", &prototype) != napi_ok ||
      napi_get_named_property(env, prototype, "releaseBuffer", &release) != napi_ok ||
      napi_typeof(env, release, &type) != napi_ok || type != napi_function) {
    return;
  }
  // Lives as long as the env: the provider frees it at release.
  napi_ref release_ref = nullptr;
  napi_value wrapper = nullptr;
  if (napi_create_reference(env, release, 1, &release_ref) != napi_ok ||
      napi_create_function(env, "releaseBuffer", NAPI_AUTO_LENGTH, SerializerReleaseBuffer,
                           release_ref, &wrapper) != napi_ok ||
      napi_set_named_property(env, prototype, "releaseBuffer", wrapper) != napi_ok) {
    if (release_ref != nullptr) napi_delete_reference(env, release_ref);
  }
}

}  // namespace

napi_value InitSerdes(napi_env env) {
  napi_value out = nullptr;
  if (unofficial_napi_create_serdes_binding(env, &out) != napi_ok || out == nullptr) {
    return Undefined(env);
  }
  WrapReleaseBuffer(env, out);
  return out;
}

}  // namespace internal_binding
