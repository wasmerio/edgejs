#ifndef EDGE_ERRORS_BINDING_H_
#define EDGE_ERRORS_BINDING_H_

#include <string>

#include "node_api.h"

napi_value EdgeGetOrCreateErrorsBinding(napi_env env);
std::string EdgeFormatFatalExceptionAfterInspector(napi_env env, napi_value exception);
// Node's errors::TriggerUncaughtException (process._fatalException, then the
// fatal path when unhandled). Returns false when the handler itself threw and
// that exception is pending. With `keep_worker_stopped`, a worker the call
// stops (its _fatalException ends it with process.exit()) stays terminated,
// so no more of the caller's JavaScript runs, as in Node; without it the
// termination is cancelled for the caller to unwind itself.
bool EdgeTriggerUncaughtException(napi_env env,
                                  napi_value exception,
                                  bool from_promise,
                                  bool keep_worker_stopped = false);

#endif  // EDGE_ERRORS_BINDING_H_
