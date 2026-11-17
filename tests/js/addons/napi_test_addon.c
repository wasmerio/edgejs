/* A Node-API addon for Edge's own JS tests (tests/CMakeLists.txt). */
#include <node_api.h>

/* fatal(error): reports `error` with napi_fatal_exception and returns. */
static napi_value Fatal(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1] = {NULL};
  if (napi_get_cb_info(env, info, &argc, argv, NULL, NULL) != napi_ok || argc < 1) return NULL;
  napi_fatal_exception(env, argv[0]);
  return NULL;
}

NAPI_MODULE_INIT(/* napi_env env, napi_value exports */) {
  napi_value fn = NULL;
  if (napi_create_function(env, "fatal", NAPI_AUTO_LENGTH, Fatal, NULL, &fn) != napi_ok ||
      napi_set_named_property(env, exports, "fatal", fn) != napi_ok) {
    return NULL;
  }
  return exports;
}
