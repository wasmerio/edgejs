#ifndef EDGE_ERRORS_BINDING_H_
#define EDGE_ERRORS_BINDING_H_

#include <string>

#include "node_api.h"

napi_value EdgeGetOrCreateErrorsBinding(napi_env env);
std::string EdgeFormatFatalExceptionAfterInspector(napi_env env, napi_value exception);
// Node's errors::TriggerUncaughtException (process._fatalException, then the
// fatal path when unhandled). Returns false when the handler itself threw and
// that exception is pending.
bool EdgeTriggerUncaughtException(napi_env env, napi_value exception, bool from_promise);

#endif  // EDGE_ERRORS_BINDING_H_
